#include "vfs/disk_state.h"

#include <cassert>
#include <utility>

namespace clice::vfs {

std::optional<std::uint64_t> DiskState::seen_hash(Fid fid) const {
    auto it = files.find(fid);
    return it != files.end() ? it->second.seen : std::nullopt;
}

bool DiskState::seen_missing(Fid fid) const {
    auto it = files.find(fid);
    return it != files.end() && !it->second.seen;
}

llvm::SmallVector<Fid> DiskState::missing_files() const {
    llvm::SmallVector<Fid> result;
    for(auto& [fid, file]: files) {
        if(!file.seen) {
            result.push_back(fid);
        }
    }
    return result;
}

llvm::SmallVector<Fid> DiskState::take_changes() {
    changed.clear();
    return std::exchange(changes, {});
}

void DiskState::saw_missing(Fid fid) {
    saw(fid, std::nullopt);
}

void DiskState::observe(Fid fid, const DiskObservation& obs) {
    saw(fid, obs.hash);
    if(obs.reliable) {
        files[fid].pair = Pair{.stamp = obs.stamp, .hash = obs.hash};
    }
}

std::optional<DiskObservation> DiskState::read(Fid fid) {
    auto observed = read_observed(path(fid));
    if(!observed) {
        return std::nullopt;
    }
    observe(fid, observed->obs);
    return observed->obs;
}

std::optional<DiskObservation> DiskState::current(Fid fid) {
    auto status = vfs::status(path(fid));
    if(!status) {
        saw_missing(fid);
        return std::nullopt;
    }
    return observe_for(fid, *status);
}

std::optional<DiskObservation> DiskState::observe_for(Fid fid, const Status& status) {
    if(auto hash = cached_hash(fid, status.stamp)) {
        return DiskObservation{.stamp = status.stamp,
                               .hash = *hash,
                               .paired = true,
                               .reliable = true};
    }
    return read(fid);
}

std::optional<std::uint64_t> DiskState::cached_hash(Fid fid, const Stamp& stamp) {
    auto it = files.find(fid);
    if(it == files.end() || !it->second.pair || it->second.pair->stamp != stamp) {
        return std::nullopt;
    }
    auto hash = it->second.pair->hash;
    saw(fid, hash);
    return hash;
}

DiskState::Wave::Wave(DiskState& state) : state(state) {
    assert(!state.wave_open && "waves do not nest");
    state.wave_open = true;
}

DiskState::Wave::~Wave() {
    state.wave_looks.clear();
    state.wave_statuses = {};
    state.wave_open = false;
}

DiskState::Verdict DiskState::check(Fid fid, std::uint64_t hash) {
    auto found = look(fid);
    switch(found.kind) {
        case Look::Missing: return Verdict::Missing;
        case Look::Unreadable: return Verdict::Unreadable;
        case Look::Read: return hash != 0 && found.hash == hash ? Verdict::Fresh : Verdict::Stale;
    }
    std::unreachable();
}

bool DiskState::present(Fid fid) {
    return look(fid).kind == Look::Read;
}

void DiskState::saw(Fid fid, std::optional<std::uint64_t> hash) {
    auto [it, first] = files.try_emplace(fid);
    auto& file = it->second;
    auto previous = std::exchange(file.seen, hash);
    if(first || previous == hash) {
        return;
    }
    if(changed.insert(fid).second) {
        changes.push_back(fid);
        if(changes.size() == 1 && on_change) {
            on_change();
        }
    }
}

DiskState::Look DiskState::look(Fid fid) {
    assert(wave_open && "a check outside a Wave");
    if(auto it = wave_looks.find(fid); it != wave_looks.end()) {
        return it->second;
    }
    Look found{.kind = Look::Missing};
    if(auto status = wave_statuses.status(path(fid)); !status) {
        saw_missing(fid);
    } else if(auto obs = observe_for(fid, *status)) {
        found = {.kind = Look::Read, .hash = obs->hash};
    } else {
        found.kind = Look::Unreadable;
    }
    wave_looks.try_emplace(fid, found);
    return found;
}

}  // namespace clice::vfs
