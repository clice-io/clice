#include "sched/claims/registry.h"

#include "llvm/Support/xxhash.h"

namespace clice {

namespace {

ContentHash to_hash(worker::Hash128 hash) {
    return {.low = hash.low, .high = hash.high};
}

}  // namespace

std::uint64_t ClaimRegistry::fingerprint_of(llvm::StringRef text) {
    return llvm::xxh3_64bits(text);
}

worker::ClaimResult ClaimRegistry::claim(std::uint64_t attempt,
                                         Fid requester,
                                         const worker::ClaimParams& params,
                                         llvm::ArrayRef<Fid> files) {
    auto fingerprint = fingerprint_of(params.fingerprint);
    auto& granted = grants[attempt];
    worker::ClaimResult result;
    result.files.resize(params.files.size());
    for(std::size_t f = 0; f < params.files.size(); f += 1) {
        auto& file = params.files[f];
        auto& runs = result.files[f].runs;
        runs.assign(file.units.size(), worker::ClaimRun::Skip);
        for(std::size_t u = 0; u < file.units.size(); u += 1) {
            auto& unit = file.units[u];
            Key key{.purpose = params.purpose,
                    .fingerprint = fingerprint,
                    .file = files[f],
                    .key = to_hash(unit.key)};
            auto& entry = entries[key];
            if(!llvm::is_contained(entry.requesters, requester)) {
                entry.requesters.push_back(requester);
            }
            Grant grant{.key = key, .unit = false};
            for(auto element: unit.elements) {
                auto hash = to_hash(element);
                entry.elements_seen.insert(hash);
                if(!entry.elements_done.contains(hash) && !entry.elements_pending.contains(hash)) {
                    entry.elements_pending.insert(hash);
                    grant.elements.push_back(hash);
                }
            }
            if(entry.state == State::Unclaimed) {
                entry.state = State::Claimed;
                entry.attempt = attempt;
                grant.unit = true;
                runs[u] = worker::ClaimRun::Full;
            } else if(!grant.elements.empty()) {
                runs[u] = worker::ClaimRun::Elements;
            }
            if(grant.unit || !grant.elements.empty()) {
                granted.push_back(std::move(grant));
            }
        }
    }
    return result;
}

void ClaimRegistry::land(std::uint64_t attempt) {
    auto it = grants.find(attempt);
    if(it == grants.end()) {
        return;
    }
    for(auto& grant: it->second) {
        auto& entry = entries[grant.key];
        if(grant.unit) {
            entry.state = State::Done;
        }
        for(auto element: grant.elements) {
            entry.elements_pending.erase(element);
            entry.elements_done.insert(element);
        }
    }
    grants.erase(it);
}

void ClaimRegistry::release(std::uint64_t attempt) {
    auto it = grants.find(attempt);
    if(it == grants.end()) {
        return;
    }
    for(auto& grant: it->second) {
        auto& entry = entries[grant.key];
        if(grant.unit && entry.state == State::Claimed && entry.attempt == attempt) {
            entry.state = State::Unclaimed;
        }
        for(auto element: grant.elements) {
            entry.elements_pending.erase(element);
        }
    }
    grants.erase(it);
}

std::vector<ClaimRegistry::Unfinished> ClaimRegistry::unfinished() const {
    std::vector<Unfinished> result;
    for(auto& [key, entry]: entries) {
        bool owed = entry.state != State::Done;
        for(auto element: entry.elements_seen) {
            owed = owed || !entry.elements_done.contains(element);
        }
        if(owed && !entry.requesters.empty()) {
            result.push_back({.key = key, .requesters = entry.requesters});
        }
    }
    return result;
}

}  // namespace clice
