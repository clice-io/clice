#include "vfs/disk_state.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <utility>

#include "support/anomaly.h"
#include "support/filesystem.h"
#include "vfs/path.h"

#include "llvm/ADT/STLExtras.h"

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
    saw(fid, std::nullopt, true);
}

void DiskState::observe(Fid fid, const DiskObservation& obs) {
    saw(fid, obs.hash, obs.reliable);
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
    saw(fid, hash, true);
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

void DiskState::add_root(llvm::StringRef dir, Policy policy) {
    if(auto it = std::ranges::find(roots, dir, &Root::dir); it != roots.end()) {
        it->policy = policy;
    } else {
        roots.push_back({.dir = dir.str(), .policy = policy});
    }
    for(auto& [fid, file]: files) {
        if(path::under(path(fid), dir)) {
            file.root = root_of(path(fid));
            schedule(fid, file, file.due);
        }
    }
}

void DiskState::consumed(Fid fid, std::uint64_t hash) {
    if(!files.contains(fid)) {
        saw(fid, hash, false);
    }
}

void DiskState::expire_under(llvm::StringRef dir) {
    auto at = now();
    for(auto& [fid, file]: files) {
        if(path::under(path(fid), dir)) {
            schedule(fid, file, at);
        }
    }
}

void DiskState::add_package(llvm::StringRef dir) {
    if(std::ranges::find(roots, dir, &Root::dir) != roots.end()) {
        return;
    }
    add_root(dir, package_policy);
    path::walk_ancestors(dir, "", [&](llvm::StringRef ancestor) {
        auto history = path::join(ancestor, "conda-meta", "history");
        if(!vfs::status(history)) {
            return true;
        }
        if(!environments.contains(ancestor)) {
            environments[ancestor] =
                watch(history, [this, environment = ancestor.str()] { expire_under(environment); });
        }
        return false;
    });
}

void DiskState::tick(Clock::duration budget) {
    look_flags();

    // Due by the schedule's clock; the budget is wall time, and every tick
    // looks at one file at least.
    auto at = now();
    auto deadline = Clock::now() + budget;
    std::size_t looked = 0;
    // Files are looked at in path order a batch at a time, so a directory
    // the batch asks about often enough is listed once (see StatusBatch).
    constexpr std::size_t batch_size = 256;
    while(true) {
        llvm::SmallVector<Fid> batch;
        while(batch.size() < batch_size && !queue.empty() && queue.front().at <= at) {
            std::ranges::pop_heap(queue, std::ranges::greater{}, &Due::at);
            auto entry = queue.back();
            queue.pop_back();
            auto& file = files.find(entry.fid)->second;
            if(file.queued != entry.at) {
                continue;
            }
            file.queued = Clock::time_point::max();
            if(file.due > at) {
                // Looked at since it was queued.
                schedule(entry.fid, file, file.due);
                continue;
            }
            batch.push_back(entry.fid);
        }
        if(batch.empty()) {
            return;
        }
        std::ranges::sort(batch, {}, [&](Fid fid) { return path(fid); });
        StatusBatch statuses;
        for(auto [index, fid]: llvm::enumerate(batch)) {
            if(looked != 0 && Clock::now() >= deadline) {
                for(auto rest: llvm::drop_begin(batch, index)) {
                    auto& file = files.find(rest)->second;
                    schedule(rest, file, file.due);
                }
                return;
            }
            looked += 1;
            look_at(fid, statuses);
            // An unreadable file records nothing: look again after its
            // interval.
            if(auto& file = files.find(fid)->second; file.queued == Clock::time_point::max()) {
                schedule(fid, file, at + file.interval);
            }
        }
    }
}

void DiskState::look(llvm::ArrayRef<Fid> fids) {
    auto sorted = llvm::to_vector(fids);
    std::ranges::sort(sorted, {}, [&](Fid fid) { return path(fid); });
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    StatusBatch statuses;
    for(auto fid: sorted) {
        look_at(fid, statuses);
    }
}

void DiskState::look_all() {
    look_flags();
    look(llvm::to_vector(llvm::make_first_range(files)));
}

std::shared_ptr<const Flag> DiskState::watch(std::string path, std::function<void()> on_change) {
    auto watch = std::make_shared<Watch>(Watch{
        .flag = {.path = std::move(path)},
        .on_change = std::move(on_change),
    });
    watch->flag.look();
    watches.push_back(watch);
    return std::shared_ptr<const Flag>(watch, &watch->flag);
}

void DiskState::saw(Fid fid, std::optional<std::uint64_t> hash, bool settled) {
    auto [it, first] = files.try_emplace(fid);
    auto& file = it->second;
    if(first) {
        file.root = root_of(path(fid));
    }
    auto previous = std::exchange(file.seen, hash);
    bool moved = !first && previous != hash;
    auto& rule = policy(file);
    file.interval = first || moved || !settled ? rule.min : std::min(file.interval * 2, rule.max);
    schedule(fid, file, now() + file.interval);
    if(!moved || !changed.insert(fid).second) {
        return;
    }
    changes.push_back(fid);
    if(changes.size() == 1 && on_change) {
        on_change();
    }
}

DiskState::Look DiskState::look(Fid fid) {
    assert(wave_open && "a check outside a Wave");
    if(auto it = wave_looks.find(fid); it != wave_looks.end()) {
        return it->second;
    }
    auto found = trusted(fid);
    if(found && shadow) {
        verify(fid, *found);
    } else if(!found) {
        found = Look{.kind = Look::Missing};
        if(auto status = wave_statuses.status(path(fid)); !status) {
            saw_missing(fid);
        } else if(auto obs = observe_for(fid, *status)) {
            found = Look{.kind = Look::Read, .hash = obs->hash};
        } else {
            found->kind = Look::Unreadable;
        }
    }
    wave_looks.try_emplace(fid, *found);
    return *found;
}

std::optional<DiskState::Look> DiskState::trusted(Fid fid) {
    auto it = files.find(fid);
    if(it == files.end()) {
        return std::nullopt;
    }
    auto& file = it->second;
    if(policy(file).kind != Class::Package || file.due <= now()) {
        return std::nullopt;
    }
    if(!file.seen) {
        return Look{.kind = Look::Missing};
    }
    return Look{.kind = Look::Read, .hash = *file.seen};
}

void DiskState::verify(Fid fid, const Look& found) {
    Look truth{.kind = Look::Missing};
    if(auto status = vfs::status(path(fid))) {
        auto& pair = files.find(fid)->second.pair;
        if(pair && pair->stamp == status->stamp) {
            truth = {.kind = Look::Read, .hash = pair->hash};
        } else if(auto observed = read_observed(path(fid))) {
            truth = {.kind = Look::Read, .hash = observed->obs.hash};
        } else {
            return;
        }
    }
    if(truth.kind != found.kind || truth.hash != found.hash) {
        auto describe = [](const Look& look) {
            return look.kind == Look::Missing ? std::string("missing")
                                              : std::format("hash {:016x}", look.hash);
        };
        LOG_ANOMALY(StaleTrust,
                    "{} was trusted as {} but holds {}",
                    path(fid),
                    describe(found),
                    describe(truth));
    }
}

std::uint32_t DiskState::root_of(llvm::StringRef path) const {
    auto found = no_root;
    for(auto [index, root]: llvm::enumerate(roots)) {
        if(path::under(path, root.dir) &&
           (found == no_root || root.dir.size() > roots[found].dir.size())) {
            found = static_cast<std::uint32_t>(index);
        }
    }
    return found;
}

void DiskState::schedule(Fid fid, File& file, Clock::time_point at) {
    file.due = at;
    if(policy(file).max == Clock::duration::zero() || at >= file.queued) {
        return;
    }
    file.queued = at;
    queue.push_back({.at = at, .fid = fid});
    std::ranges::push_heap(queue, std::ranges::greater{}, &Due::at);
}

void DiskState::look_flags() {
    // An owner may watch or drop flags when it hears of a change.
    std::erase_if(watches, [](const std::weak_ptr<Watch>& watch) { return watch.expired(); });
    llvm::SmallVector<std::shared_ptr<Watch>> live;
    for(auto& watch: watches) {
        live.push_back(watch.lock());
    }
    for(auto& watch: live) {
        if(watch->flag.look()) {
            watch->on_change();
        }
    }
}

void DiskState::look_at(Fid fid, StatusBatch& statuses) {
    if(auto status = statuses.status(path(fid)); !status) {
        saw_missing(fid);
    } else {
        observe_for(fid, *status);
    }
}

bool Flag::look() {
    std::optional<std::uint64_t> found;
    auto status = vfs::status(path);
    if(status && stamp == status->stamp) {
        found = hash;
    } else {
        stamp.reset();
        if(status) {
            if(auto observed = read_observed(path)) {
                found = observed->obs.hash;
                if(observed->obs.reliable) {
                    stamp = observed->obs.stamp;
                }
            }
        }
    }
    bool was_missing = std::exchange(missing, !status);
    return std::exchange(hash, found) != found || was_missing != missing;
}

}  // namespace clice::vfs
