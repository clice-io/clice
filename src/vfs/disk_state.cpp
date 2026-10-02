#include "vfs/disk_state.h"

#include <algorithm>
#include <format>
#include <utility>

#include "support/anomaly.h"
#include "vfs/path.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

namespace clice::vfs {

std::optional<std::uint64_t> DiskState::seen_hash(Fid fid) const {
    auto it = files.find(fid);
    return it != files.end() ? it->second.seen : std::nullopt;
}

bool DiskState::seen_missing(Fid fid) const {
    auto it = files.find(fid);
    return it != files.end() && !it->second.seen;
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
        // The file is there but could not be read: nothing is recorded,
        // and nothing is answered from the last look until a read succeeds.
        if(auto it = files.find(fid); it != files.end()) {
            it->second.due = std::min(it->second.due, now());
        }
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

void DiskState::end_turn() {
    turn_looks.clear();
    turn_statuses = {};
    turn_open = false;
}

kota::task<> DiskState::end_turns(kota::event_loop& loop) {
    auto before_io = kota::prepare::create(loop);
    on_turn = [&before_io] {
        before_io.start();
    };
    auto unwired = llvm::make_scope_exit([this] { on_turn = {}; });
    while(true) {
        co_await before_io.wait();
        before_io.stop();
        end_turn();
    }
}

DiskState::Verdict DiskState::check(Fid fid, std::uint64_t hash) {
    auto expected =
        hash != 0 ? std::optional(Look{.found = Look::Found::Read, .hash = hash}) : std::nullopt;
    auto look = turn_look(fid, expected);
    switch(look.found) {
        case Look::Found::Missing: return Verdict::Missing;
        case Look::Found::Unreadable: return Verdict::Unreadable;
        case Look::Found::Read: return look == expected ? Verdict::Fresh : Verdict::Stale;
    }
    std::unreachable();
}

bool DiskState::present(Fid fid) {
    return turn_look(fid, Look{.found = Look::Found::Missing}).found == Look::Found::Read;
}

void DiskState::add_root(llvm::StringRef dir, Policy rule) {
    if(auto it = std::ranges::find(roots, dir, &Root::dir); it != roots.end()) {
        it->policy = rule;
    } else {
        roots.push_back({.dir = dir.str(), .policy = rule});
    }
    for(auto& [fid, file]: files) {
        if(path::under(path(fid), dir)) {
            // Under another policy the backoff starts over.
            file.root = root_of(path(fid));
            file.interval = policy(file).min;
            schedule(fid, file, std::min(file.due, now() + file.interval));
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
    std::size_t popped = 0;
    // Files are looked at in path order a batch at a time, so a directory
    // the batch asks about often enough is listed once (see StatusBatch).
    constexpr std::size_t batch_size = 256;
    while(true) {
        llvm::SmallVector<Fid> batch;
        while(batch.size() < batch_size && !queue.empty() && queue.front().at <= at) {
            // A burst of expiries leaves leftovers behind: discarding them
            // spends the budget too.
            if(popped != 0 && popped % batch_size == 0 && Clock::now() >= deadline) {
                break;
            }
            popped += 1;
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
            if(auto& file = files.find(fid)->second; file.queued == Clock::time_point::max()) {
                // Nothing recorded: the read failed. Retry soon.
                enqueue(fid, file, at + policy(file).min);
            }
        }
    }
}

void DiskState::look(llvm::ArrayRef<Fid> fids) {
    auto sorted = llvm::to_vector(fids);
    std::ranges::sort(sorted, {}, [&](Fid fid) { return path(fid); });
    sorted.erase(llvm::unique(sorted), sorted.end());
    StatusBatch statuses;
    for(auto fid: sorted) {
        look_at(fid, statuses);
    }
}

void DiskState::find_missing(llvm::ArrayRef<Fid> fids) {
    auto sorted = llvm::to_vector(fids);
    std::ranges::sort(sorted, {}, [&](Fid fid) { return path(fid); });
    StatusBatch statuses;
    for(auto fid: sorted) {
        if(!statuses.status(path(fid))) {
            saw_missing(fid);
        }
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
    if(!hash) {
        file.pair.reset();
    }
    if(turn_open) {
        turn_looks.insert_or_assign(fid,
                                    hash ? Look{.found = Look::Found::Read, .hash = *hash}
                                         : Look{.found = Look::Found::Missing});
    }
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

DiskState::Look DiskState::turn_look(Fid fid, std::optional<Look> expected) {
    if(!on_turn) {
        end_turn();
    } else if(!turn_open) {
        turn_open = true;
        on_turn();
    }
    if(auto it = turn_looks.find(fid); it != turn_looks.end()) {
        return it->second;
    }
    // Trust only confirms: a stale last look must never stand for a change
    // the disk may not hold (a build may have read newer bytes than it).
    if(auto last = trusted(fid); last && last == expected) {
        checks.trusted += 1;
        if(shadow) {
            verify(fid, *last);
        }
        return *last;
    }
    checks.looked += 1;
    Look look{.found = Look::Found::Missing};
    if(auto status = turn_statuses.status(path(fid)); !status) {
        saw_missing(fid);
    } else if(auto obs = observe_for(fid, *status)) {
        look = {.found = Look::Found::Read, .hash = obs->hash};
    } else {
        look.found = Look::Found::Unreadable;
    }
    turn_looks.try_emplace(fid, look);
    return look;
}

std::optional<DiskState::Look> DiskState::trusted(Fid fid) {
    auto it = files.find(fid);
    if(it == files.end()) {
        return std::nullopt;
    }
    auto& file = it->second;
    if(policy(file).kind != Kind::Package || file.due <= now()) {
        return std::nullopt;
    }
    if(!file.seen) {
        return Look{.found = Look::Found::Missing};
    }
    return Look{.found = Look::Found::Read, .hash = *file.seen};
}

void DiskState::verify(Fid fid, const Look& found) {
    Look truth{.found = Look::Found::Missing};
    if(auto status = vfs::status(path(fid))) {
        auto& pair = files.find(fid)->second.pair;
        if(pair && pair->stamp == status->stamp) {
            truth = {.found = Look::Found::Read, .hash = pair->hash};
        } else if(auto observed = read_observed(path(fid))) {
            truth = {.found = Look::Found::Read, .hash = observed->obs.hash};
        } else {
            return;
        }
    }
    if(truth != found) {
        auto describe = [](const Look& look) {
            return look.found == Look::Found::Missing ? std::string("missing")
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
    enqueue(fid, file, at);
}

void DiskState::enqueue(Fid fid, File& file, Clock::time_point at) {
    if(policy(file).max == Clock::duration::zero() || at >= file.queued) {
        return;
    }
    file.queued = at;
    queue.push_back({.at = at, .fid = fid});
    std::ranges::push_heap(queue, std::ranges::greater{}, &Due::at);
}

void DiskState::look_flags() {
    // An owner may watch or drop flags when it hears of a change.
    llvm::erase_if(watches, [](const std::weak_ptr<Watch>& watch) { return watch.expired(); });
    llvm::SmallVector<std::shared_ptr<Watch>> live;
    for(auto& watch: watches) {
        live.push_back(watch.lock());
    }
    for(auto& watch: live) {
        if(watch->flag.look() && watch->on_change) {
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
    auto status = vfs::status(path);
    std::optional<std::uint64_t> found;
    if(status && hashed == status->stamp) {
        found = hash;
    } else {
        hashed.reset();
        if(status && status->type != llvm::sys::fs::file_type::directory_file) {
            if(auto observed = read_observed(path)) {
                found = observed->obs.hash;
                if(observed->obs.reliable) {
                    hashed = observed->obs.stamp;
                }
            }
        }
    }
    auto now = status ? std::optional(status->stamp) : std::nullopt;
    bool moved = std::exchange(stamp, now) != now;
    return std::exchange(hash, found) != found || moved;
}

}  // namespace clice::vfs
