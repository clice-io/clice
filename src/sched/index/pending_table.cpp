module;

#include "support/prelude.h"

module clice;

import :sched.index.pending_table;

namespace clice {

bool PendingTable::record(Fid id, ReindexReason reason) {
    // A fresh slot means any prior slot was already consumed (or none
    // existed); a queued-and-unconsumed slot makes this call a duplicate.
    bool fresh_slot = queued.insert(id).second;

    // Within one queued slot ContentChanged is absorbing: a deps-only
    // cascade cannot downgrade a file whose own content already changed.
    // Across slots it is not: a deps-only requeue after the previous slot
    // was consumed is new debt of its own kind — the in-flight (or
    // finished) pass already covers the earlier content change, and
    // keeping ContentChanged would suppress the file's index past that
    // pass. The fresh ticket invalidates the finish of any attempt
    // already in flight for this file.
    ticket += 1;
    auto [it, inserted] = entries.try_emplace(id,
                                              reason,
                                              ticket,
                                              reason == ReindexReason::ContentChanged ? ticket : 0);
    if(!inserted) {
        if(reason == ReindexReason::ContentChanged) {
            it->second.reason = ReindexReason::ContentChanged;
            it->second.content_ticket = ticket;
            it->second.requeue_attempts = 0;
        } else if(fresh_slot) {
            it->second.reason = ReindexReason::DepsOnly;
        }
        it->second.ticket = ticket;
    }

    return fresh_slot;
}

std::optional<PendingTable::TakenDebt> PendingTable::take(Fid id) {
    queued.erase(id);
    auto it = entries.find(id);
    if(it == entries.end()) {
        return std::nullopt;
    }
    return TakenDebt{id, it->second.ticket};
}

std::optional<PendingTable::TakenDebt> PendingTable::peek(Fid id) const {
    auto it = entries.find(id);
    if(it == entries.end()) {
        return std::nullopt;
    }
    return TakenDebt{id, it->second.ticket};
}

void PendingTable::finish(const TakenDebt& taken) {
    if(auto it = entries.find(taken.id); it != entries.end() && it->second.ticket == taken.ticket) {
        entries.erase(it);
    }
}

bool PendingTable::superseded(const TakenDebt& taken) const {
    auto it = entries.find(taken.id);
    return it == entries.end() || it->second.content_ticket > taken.ticket;
}

PendingTable::FailureOutcome PendingTable::on_dispatch_failure(const TakenDebt& taken,
                                                               Failure failure) {
    // Only while the entry survives: a file removed from disk mid-flight
    // was cleared and has nothing to redo.
    auto it = entries.find(taken.id);
    if(it == entries.end()) {
        return {FailureDecision::Dropped};
    }

    // The failed dispatch carried bytes a ContentChanged recording has
    // since replaced: its crash says nothing about the fixed content, so
    // it neither spends the fresh budget nor requeues — the newer
    // content's own booking redoes the work.
    if(it->second.content_ticket > taken.ticket) {
        return {FailureDecision::Superseded};
    }

    if(failure == Failure::Crashed ||
       (failure == Failure::Lost && it->second.requeue_attempts >= max_requeue_attempts)) {
        // Giving up accepts the staleness, so clear the table state
        // here rather than relying on the attempt's ticket-guarded
        // finish: a deps-only recording that arrived mid-flight bumped
        // the ticket, and the guard would leave that downgraded entry
        // booked — a doomed retry that spends one more worker.
        clear(taken.id);
        return {FailureDecision::GaveUp};
    }
    if(failure == Failure::Lost) {
        it->second.requeue_attempts += 1;
    }

    // Requeue with the debt class this dispatch carried, not the entry's
    // current one; record() resets the budget on ContentChanged — right
    // for a user edit, wrong for this requeue of the same bytes — so
    // restore the table entry afterwards.
    auto reason = it->second.content_ticket == taken.ticket ? ReindexReason::ContentChanged
                                                            : it->second.reason;
    auto attempts = it->second.requeue_attempts;
    bool needs_slot = record(taken.id, reason);
    entries.find(taken.id)->second.requeue_attempts = attempts;
    return {FailureDecision::Requeued, needs_slot};
}

llvm::SmallVector<Fid> PendingTable::pending_files() const {
    llvm::SmallVector<Fid> files;
    files.reserve(entries.size());
    for(auto id: views::transform(entries, [](auto& entry) -> auto& { return entry.first; })) {
        files.push_back(id);
    }
    return files;
}

}  // namespace clice
