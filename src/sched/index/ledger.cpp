#include "sched/index/ledger.h"

#include "llvm/ADT/STLExtras.h"

namespace clice {

bool PendingLedger::record(std::uint32_t id, ReindexReason reason) {
    // A fresh slot means any prior slot was already consumed (or none
    // existed); a queued-and-unconsumed slot makes this call a duplicate.
    bool fresh_slot = queued.insert(id).second;

    // Within one queued slot ContentChanged is absorbing: a deps-only
    // cascade cannot downgrade a file whose own content already changed.
    // Across slots it is not: a deps-only requeue after the previous slot
    // was consumed is new debt of its own kind — the in-flight (or
    // finished) pass already covers the earlier content change, and
    // keeping ContentChanged would suppress the file's rows past that
    // pass. The fresh ticket invalidates the settle of any attempt
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

std::optional<PendingLedger::Claim> PendingLedger::claim(std::uint32_t id) {
    queued.erase(id);
    auto it = entries.find(id);
    if(it == entries.end()) {
        return std::nullopt;
    }
    return Claim{id, it->second.ticket};
}

std::optional<PendingLedger::Claim> PendingLedger::peek(std::uint32_t id) const {
    auto it = entries.find(id);
    if(it == entries.end()) {
        return std::nullopt;
    }
    return Claim{id, it->second.ticket};
}

void PendingLedger::settle(const Claim& claim) {
    if(auto it = entries.find(claim.id); it != entries.end() && it->second.ticket == claim.ticket) {
        entries.erase(it);
    }
}

bool PendingLedger::superseded(const Claim& claim) const {
    auto it = entries.find(claim.id);
    return it == entries.end() || it->second.content_ticket > claim.ticket;
}

PendingLedger::FailureOutcome PendingLedger::on_dispatch_failure(const Claim& claim, bool crashed) {
    // Only while the entry survives: a file removed from disk mid-flight
    // was cleared and has nothing to redo.
    auto it = entries.find(claim.id);
    if(it == entries.end()) {
        return {FailureVerdict::Dropped};
    }

    // The failed dispatch carried bytes a ContentChanged recording has
    // since replaced: its crash says nothing about the fixed content, so
    // it neither spends the fresh budget nor requeues — the newer
    // content's own booking redoes the work.
    if(it->second.content_ticket > claim.ticket) {
        return {FailureVerdict::Superseded};
    }

    if(crashed) {
        if(it->second.requeue_attempts >= max_requeue_attempts) {
            // Giving up accepts the staleness, so clear the ledger state
            // here rather than relying on the attempt's ticket-guarded
            // settle: a deps-only recording that landed mid-flight bumped
            // the ticket, and the guard would leave that downgraded entry
            // booked — a doomed retry that spends one more worker.
            clear(claim.id);
            return {FailureVerdict::GaveUp};
        }
        it->second.requeue_attempts += 1;
    }

    // Requeue with the debt class this dispatch carried, not the entry's
    // current one; record() resets the budget on ContentChanged — right
    // for a user edit, wrong for this requeue of the same bytes — so
    // restore the ledger afterwards.
    auto reason = it->second.content_ticket == claim.ticket ? ReindexReason::ContentChanged
                                                            : it->second.reason;
    auto attempts = it->second.requeue_attempts;
    bool needs_slot = record(claim.id, reason);
    entries.find(claim.id)->second.requeue_attempts = attempts;
    return {FailureVerdict::Requeued, needs_slot};
}

llvm::SmallVector<std::uint32_t> PendingLedger::pending_files() const {
    llvm::SmallVector<std::uint32_t> files;
    files.reserve(entries.size());
    for(auto id: llvm::make_first_range(entries)) {
        files.push_back(id);
    }
    return files;
}

}  // namespace clice
