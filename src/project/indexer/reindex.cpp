module clice;

import :project.indexer;

namespace clice {

void ReindexQueue::record(Fid id, ReindexReason reason) {
    // A fresh place means any prior place was already consumed (or none
    // existed); a queued-and-unconsumed place makes this call a duplicate.
    bool fresh_place = queued_files.insert(id).second;

    // Within one queued place ContentChanged is absorbing: a deps-only
    // cascade cannot downgrade a file whose own content already changed.
    // Across places it is not: a deps-only requeue after the previous place
    // was consumed is a new pending reindex of its own kind — the in-flight (or
    // finished) run already covers the earlier content change, and
    // keeping ContentChanged would suppress the file's index past that
    // run. The fresh sequence number invalidates the finish of any attempt
    // already in flight for this file.
    seq += 1;
    auto [it, inserted] =
        entries.try_emplace(id, reason, seq, reason == ReindexReason::ContentChanged ? seq : 0);
    if(!inserted) {
        if(reason == ReindexReason::ContentChanged) {
            it->second.reason = ReindexReason::ContentChanged;
            it->second.content_seq = seq;
            it->second.requeues = 0;
        } else if(fresh_place) {
            it->second.reason = ReindexReason::DepsOnly;
        }
        it->second.seq = seq;
    }
    if(fresh_place) {
        order.push_back(id);
    }
}

void ReindexQueue::move_to_front(Fid id) {
    auto it = std::find(order.begin() + position, order.end(), id);
    if(it == order.end()) {
        return;
    }
    auto past_pass = it >= order.begin() + pass_end;
    std::rotate(order.begin() + (past_pass ? pass_end : position), it, it + 1);
}

std::size_t ReindexQueue::begin_pass(llvm::function_ref<bool(Fid)> first) {
    std::stable_partition(order.begin() + position, order.end(), first);
    pass_end = order.size();
    return pass_end - position;
}

std::optional<ReindexQueue::TakenReindex> ReindexQueue::take_next() {
    while(position < pass_end) {
        auto id = order[position];
        position += 1;
        queued_files.erase(id);
        if(auto it = entries.find(id); it != entries.end()) {
            return TakenReindex{id, it->second.seq};
        }
    }
    return std::nullopt;
}

void ReindexQueue::end_pass() {
    pass_end = position;
    if(position < order.size()) {
        return;
    }
    // A pending reindex legitimately outlives the pass (one the graph
    // refused keeps its booking); a queued place must not.
    assert(queued_files.empty() && "a drained queue must leave no unconsumed place");
    order.clear();
    position = 0;
    pass_end = 0;
}

std::optional<ReindexQueue::TakenReindex> ReindexQueue::peek(Fid id) const {
    auto it = entries.find(id);
    if(it == entries.end()) {
        return std::nullopt;
    }
    return TakenReindex{id, it->second.seq};
}

void ReindexQueue::finish(const TakenReindex& taken) {
    if(auto it = entries.find(taken.id); it != entries.end() && it->second.seq == taken.seq) {
        entries.erase(it);
    }
}

bool ReindexQueue::outdated(const TakenReindex& taken) const {
    auto it = entries.find(taken.id);
    return it == entries.end() || it->second.content_seq > taken.seq;
}

ReindexQueue::FailureDecision ReindexQueue::on_dispatch_failure(const TakenReindex& taken,
                                                                Failure failure) {
    // Only while the entry survives: a file removed from disk mid-flight
    // was cleared and has nothing to redo.
    auto it = entries.find(taken.id);
    if(it == entries.end()) {
        return FailureDecision::Dropped;
    }

    // The failed dispatch carried bytes a ContentChanged recording has
    // since replaced: its crash says nothing about the fixed content, so
    // it neither spends the fresh budget nor requeues — the newer
    // content's own booking redoes the work.
    if(it->second.content_seq > taken.seq) {
        return FailureDecision::Outdated;
    }

    if(failure == Failure::Crashed ||
       (failure == Failure::Lost && it->second.requeues >= max_requeues)) {
        // Giving up accepts the staleness, so clear the pending state
        // here rather than relying on the attempt's sequence-guarded
        // finish: a deps-only recording that arrived mid-flight bumped
        // the sequence number, and the guard would leave that downgraded entry
        // booked — a doomed retry that spends one more worker.
        clear(taken.id);
        return FailureDecision::GaveUp;
    }
    if(failure == Failure::Lost) {
        it->second.requeues += 1;
    }

    // Requeue with the reason this dispatch carried, not the entry's
    // current one; record() resets the budget on ContentChanged — right
    // for a user edit, wrong for this requeue of the same bytes — so
    // restore the entry afterwards.
    auto reason =
        it->second.content_seq == taken.seq ? ReindexReason::ContentChanged : it->second.reason;
    auto requeues = it->second.requeues;
    record(taken.id, reason);
    entries.find(taken.id)->second.requeues = requeues;
    return FailureDecision::Requeued;
}

}  // namespace clice
