#include "support/cache_store.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <format>
#include <mutex>
#include <vector>

#ifdef _WIN32
// The only direct windows.h include in clice.  The defines keep it from
// spilling the min/max macros (and other clutter) that break LLVM and
// standard headers; any future include of windows.h needs the same guards.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"

namespace clice {

namespace {

/// Manifest checkpoint is forced after this many commits/invalidates.
constexpr std::uint32_t checkpoint_interval = 16;

/// JSON layout of manifest.json.  Only an acceleration structure: blob
/// presence and size always come from the filesystem; the manifest merely
/// carries last-accessed times across restarts.
struct ManifestEntry {
    std::string ns;
    std::string key;
    std::int64_t atime = 0;
};

struct ManifestData {
    std::vector<ManifestEntry> entries;
};

bool is_pid_alive(std::uint32_t pid) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!handle) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0;
    bool alive = GetExitCodeProcess(handle, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    if(::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

/// Flush a file's data to stable storage before the commit rename, so a
/// crash cannot leave a blob whose name promises content it doesn't have.
/// Opened via LLVM so UTF-8 paths work on Windows.
std::error_code sync_file(llvm::StringRef path) {
    auto file = llvm::sys::fs::openNativeFile(path,
                                              llvm::sys::fs::CD_OpenExisting,
                                              llvm::sys::fs::FA_Write,
                                              llvm::sys::fs::OF_None);
    if(!file) {
        return llvm::errorToErrorCode(file.takeError());
    }

    std::error_code ec;
#ifdef _WIN32
    if(!FlushFileBuffers(*file)) {
        ec = std::error_code(GetLastError(), std::system_category());
    }
#else
    if(::fsync(*file) != 0) {
        ec = std::error_code(errno, std::generic_category());
    }
#endif
    llvm::sys::fs::closeFile(*file);
    return ec;
}

/// On a rename collision, check whether the existing destination blob is
/// byte-identical to the one we tried to publish.  This path is rare even
/// on Windows: llvm::sys::fs::rename already moves an open destination
/// aside when its holder granted delete sharing (LLVM-opened files always
/// do), so a collision means a non-cooperating process (AV scanner,
/// indexing service) holds the blob.  The extra reads stay off hot paths.
bool same_content(llvm::StringRef tmp_path, llvm::StringRef final_path) {
    llvm::sys::fs::file_status tmp_status, final_status;
    if(llvm::sys::fs::status(tmp_path, tmp_status) ||
       llvm::sys::fs::status(final_path, final_status) ||
       final_status.type() != llvm::sys::fs::file_type::regular_file ||
       tmp_status.getSize() != final_status.getSize()) {
        return false;
    }

    auto tmp_buf = llvm::MemoryBuffer::getFile(tmp_path);
    auto final_buf = llvm::MemoryBuffer::getFile(final_path);
    if(!tmp_buf || !final_buf) {
        return false;
    }
    return (*tmp_buf)->getBuffer() == (*final_buf)->getBuffer();
}

/// Remove directory children whose name parses as a dead pid.
/// Used for both `tmp/{pid}` and Scratch `{ns}/{pid}` layouts.
void sweep_dead_pid_dirs(llvm::StringRef dir, std::uint32_t self_pid) {
    std::error_code ec;
    for(auto it = llvm::sys::fs::directory_iterator(dir, ec);
        !ec && it != llvm::sys::fs::directory_iterator();
        it.increment(ec)) {
        std::uint32_t pid = 0;
        auto name = path::filename(it->path());
        if(name.getAsInteger(10, pid)) {
            continue;
        }
        if(pid == self_pid || is_pid_alive(pid)) {
            continue;
        }
        fs::remove_all(it->path());
        LOG_DEBUG("CacheStore: removed dead instance directory {}", it->path());
    }
}

std::int64_t now_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace

struct CacheStore::State {
    struct Entry {
        std::uint64_t size = 0;
        std::int64_t atime = 0;
    };

    struct Namespace {
        CacheNamespace config;
        /// Directory holding this namespace's blobs: `{base}/{name}` for
        /// LRU/Persistent, `{base}/{name}/{pid}` for Scratch.
        std::string dir;
        llvm::StringMap<Entry> entries;
        std::uint64_t total_size = 0;
    };

    std::mutex mutex;

    /// `{root}/cache/v{version}` — everything the store manages lives here.
    std::string base;

    /// `{base}/tmp/{pid}` — this instance's in-flight writes.
    std::string tmp_dir;

    std::uint32_t self_pid = 0;

    llvm::StringMap<Namespace> namespaces;

    /// Last-accessed times loaded from the manifest, keyed by "{ns}/{key}".
    /// Consumed when the corresponding namespace is registered.
    llvm::StringMap<std::int64_t> manifest_atimes;

    std::uint64_t next_tmp_id = 0;

    std::uint32_t changes_since_checkpoint = 0;
    bool dirty = false;

    /// Logical clock: strictly increasing per issued stamp so that LRU
    /// ordering is deterministic even within one millisecond.
    std::int64_t last_stamp = 0;

    std::int64_t next_stamp() {
        last_stamp = std::max(now_ms(), last_stamp + 1);
        return last_stamp;
    }

    std::string blob_path(const Namespace& ns, llvm::StringRef key) const {
        return path::join(ns.dir, key.str() + ns.config.extension);
    }

    Namespace* find_namespace(llvm::StringRef name) {
        auto it = namespaces.find(name);
        return it != namespaces.end() ? &it->second : nullptr;
    }

    void evict_locked(Namespace& ns, llvm::StringRef keep_key);
    void checkpoint_locked();
};

CacheStore::CacheStore(std::unique_ptr<State> state) : state(std::move(state)) {}

CacheStore::CacheStore(CacheStore&&) noexcept = default;

CacheStore& CacheStore::operator=(CacheStore&&) noexcept = default;

CacheStore::~CacheStore() = default;

std::expected<CacheStore, std::error_code> CacheStore::open(llvm::StringRef root,
                                                            std::uint32_t version) {
    assert(!root.empty() && "cache root must not be empty");

    auto state = std::make_unique<State>();
    state->self_pid = static_cast<std::uint32_t>(llvm::sys::Process::getProcessId());

    auto parent = path::join(root, "cache");
    auto version_dir = std::format("v{}", version);
    state->base = path::join(parent, version_dir);

    if(auto ec = llvm::sys::fs::create_directories(state->base)) {
        return std::unexpected(ec);
    }

    // Discard anything that isn't the current layout version: older version
    // directories and any stray files.
    std::error_code ec;
    for(auto it = llvm::sys::fs::directory_iterator(parent, ec);
        !ec && it != llvm::sys::fs::directory_iterator();
        it.increment(ec)) {
        if(path::filename(it->path()) == version_dir) {
            continue;
        }
        LOG_INFO("CacheStore: discarding stale cache layout {}", it->path());
        if(llvm::sys::fs::is_directory(it->path())) {
            fs::remove_all(it->path());
        } else {
            llvm::sys::fs::remove(it->path());
        }
    }

    // Load the manifest.  Corrupt or missing is fine: registration falls
    // back to a directory scan with mtimes as last-accessed times.
    auto manifest_path = path::join(state->base, "manifest.json");
    if(auto content = fs::read(manifest_path)) {
        ManifestData data;
        if(kota::codec::json::from_json(*content, data)) {
            for(auto& entry: data.entries) {
                auto key = entry.ns + "/" + entry.key;
                state->manifest_atimes[key] = entry.atime;
                state->last_stamp = std::max(state->last_stamp, entry.atime);
            }
        } else {
            LOG_WARN("CacheStore: corrupt manifest {}, rebuilding from directory scan",
                     manifest_path);
        }
    }

    // The only crash residue are in-flight tmp files; committed blobs are
    // complete by construction (atomic rename).  Sweep tmp directories of
    // instances that no longer exist.
    auto tmp_parent = path::join(state->base, "tmp");
    sweep_dead_pid_dirs(tmp_parent, state->self_pid);

    state->tmp_dir = path::join(tmp_parent, std::to_string(state->self_pid));
    fs::remove_all(state->tmp_dir);
    if(auto ec2 = llvm::sys::fs::create_directories(state->tmp_dir)) {
        return std::unexpected(ec2);
    }

    return CacheStore(std::move(state));
}

void CacheStore::register_namespace(CacheNamespace ns) {
    std::lock_guard guard(state->mutex);

    auto ns_dir = path::join(state->base, ns.name);
    llvm::sys::fs::create_directories(ns_dir);

    auto [it, inserted] = state->namespaces.try_emplace(ns.name);
    assert(inserted && "namespace registered twice");
    if(!inserted) {
        return;
    }
    auto& ns_state = it->second;
    ns_state.config = std::move(ns);

    if(ns_state.config.policy == CachePolicy::Scratch) {
        // Scratch directories are per-instance; reclaim those left behind
        // by crashed instances and start with a fresh one of our own.
        sweep_dead_pid_dirs(ns_dir, state->self_pid);
        ns_state.dir = path::join(ns_dir, std::to_string(state->self_pid));
        fs::remove_all(ns_state.dir);
        llvm::sys::fs::create_directories(ns_state.dir);
        return;
    }

    ns_state.dir = std::move(ns_dir);

    // Adopt blobs already on disk.  The directory scan, not the manifest,
    // decides existence: this also picks up blobs committed after the last
    // checkpoint of a crashed instance.
    std::error_code ec;
    for(auto iter = llvm::sys::fs::directory_iterator(ns_state.dir, ec);
        !ec && iter != llvm::sys::fs::directory_iterator();
        iter.increment(ec)) {
        auto filename = path::filename(iter->path());
        auto& ext = ns_state.config.extension;
        if(!ext.empty() && !filename.consume_back(ext)) {
            continue;
        }

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(iter->path(), status) ||
           status.type() != llvm::sys::fs::file_type::regular_file) {
            continue;
        }

        auto atime_it = state->manifest_atimes.find(ns_state.config.name + "/" + filename.str());
        auto atime = atime_it != state->manifest_atimes.end()
                         ? atime_it->second
                         : std::chrono::duration_cast<std::chrono::milliseconds>(
                               status.getLastModificationTime().time_since_epoch())
                               .count();

        ns_state.entries[filename] = {status.getSize(), atime};
        ns_state.total_size += status.getSize();
    }

    // Enforce the budget immediately in case it shrank since the last run.
    state->evict_locked(ns_state, "");
}

std::optional<std::string> CacheStore::lookup(llvm::StringRef ns, llvm::StringRef key) {
    std::lock_guard guard(state->mutex);

    auto* ns_state = state->find_namespace(ns);
    if(!ns_state) {
        return std::nullopt;
    }

    auto it = ns_state->entries.find(key);
    if(it == ns_state->entries.end()) {
        return std::nullopt;
    }

    it->second.atime = state->next_stamp();
    state->dirty = true;
    return state->blob_path(*ns_state, key);
}

CacheStore::PendingEntry CacheStore::begin_store(llvm::StringRef ns, llvm::StringRef key) {
    std::lock_guard guard(state->mutex);

    auto* ns_state = state->find_namespace(ns);
    assert(ns_state && "begin_store on unregistered namespace");
    if(!ns_state) {
        LOG_ERROR("CacheStore: begin_store on unregistered namespace {}", ns);
        return {};
    }

    auto tmp_name = std::format("{}{}", state->next_tmp_id++, ns_state->config.extension);
    return PendingEntry{ns.str(), key.str(), path::join(state->tmp_dir, tmp_name)};
}

std::expected<std::string, std::error_code> CacheStore::commit(PendingEntry pending) {
    if(pending.tmp_path.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    llvm::sys::fs::file_status status;
    if(auto ec = llvm::sys::fs::status(pending.tmp_path, status)) {
        return std::unexpected(ec);
    }

    // Scratch blobs are cheap derivatives with no durability requirement;
    // they skip the fsync.
    bool durable;
    {
        std::lock_guard guard(state->mutex);
        auto* ns_state = state->find_namespace(pending.ns);
        if(!ns_state) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        durable = ns_state->config.policy != CachePolicy::Scratch;
    }

    // fsync outside the lock so lookups are not blocked behind disk flushes.
    if(durable) {
        if(auto ec = sync_file(pending.tmp_path)) {
            llvm::sys::fs::remove(pending.tmp_path);
            return std::unexpected(ec);
        }
    }

    std::string final_path;
    {
        std::lock_guard guard(state->mutex);

        auto* ns_state = state->find_namespace(pending.ns);
        if(!ns_state) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        final_path = state->blob_path(*ns_state, pending.key);
        if(auto result = fs::rename(pending.tmp_path, final_path); !result) {
            if(same_content(pending.tmp_path, final_path)) {
                // Benign collision: an identical blob is already published
                // (Windows, destination currently open).  Keep the survivor
                // and account for it.  This is verified by comparison, not
                // assumed from the key: even LRU keys are not fully
                // content-addressed (a dependency edit changes the PCH
                // content without changing its key input).
                llvm::sys::fs::remove(pending.tmp_path);
                if(llvm::sys::fs::status(final_path, status)) {
                    return std::unexpected(result.error());
                }
            } else {
                // The destination is stale — a rewritten mutable key
                // (Persistent/Scratch) or an LRU blob whose content drifted
                // from its key.  Remove it and retry; if the rename still
                // fails, report the error instead of silently dropping the
                // new data.
                llvm::sys::fs::remove(final_path);
                if(auto retry = fs::rename(pending.tmp_path, final_path); !retry) {
                    llvm::sys::fs::remove(pending.tmp_path);
                    if(auto it = ns_state->entries.find(pending.key);
                       it != ns_state->entries.end() && llvm::sys::fs::status(final_path, status)) {
                        // The old blob is gone as well: drop its entry so
                        // lookups don't hand out a dangling path.
                        ns_state->total_size -= it->second.size;
                        ns_state->entries.erase(it);
                        state->dirty = true;
                    }
                    return std::unexpected(retry.error());
                }
            }
        }

        auto& entry = ns_state->entries[pending.key];
        // Unsigned wraparound is intentional and exact here: entry.size is
        // already included in total_size, so total + new - old stays correct
        // even when the replacement blob is smaller.
        ns_state->total_size += status.getSize() - entry.size;
        entry.size = status.getSize();
        entry.atime = state->next_stamp();

        if(ns_state->config.policy == CachePolicy::LRU) {
            state->evict_locked(*ns_state, pending.key);
        }

        if(ns_state->config.policy != CachePolicy::Scratch) {
            state->dirty = true;
            state->changes_since_checkpoint += 1;
        }
    }

    maybe_checkpoint();
    return final_path;
}

void CacheStore::abort(const PendingEntry& pending) {
    if(!pending.tmp_path.empty()) {
        llvm::sys::fs::remove(pending.tmp_path);
    }
}

void CacheStore::invalidate(llvm::StringRef ns, llvm::StringRef key) {
    {
        std::lock_guard guard(state->mutex);

        auto* ns_state = state->find_namespace(ns);
        if(!ns_state) {
            return;
        }

        auto it = ns_state->entries.find(key);
        if(it == ns_state->entries.end()) {
            return;
        }

        llvm::sys::fs::remove(state->blob_path(*ns_state, key));
        ns_state->total_size -= it->second.size;
        ns_state->entries.erase(it);

        if(ns_state->config.policy != CachePolicy::Scratch) {
            state->dirty = true;
            state->changes_since_checkpoint += 1;
        }
    }

    maybe_checkpoint();
}

void CacheStore::for_each_key(llvm::StringRef ns, llvm::function_ref<void(llvm::StringRef)> fn) {
    llvm::SmallVector<std::string> keys;
    {
        std::lock_guard guard(state->mutex);
        auto* ns_state = state->find_namespace(ns);
        if(!ns_state) {
            return;
        }
        keys.reserve(ns_state->entries.size());
        for(auto& entry: ns_state->entries) {
            keys.push_back(entry.first().str());
        }
    }

    for(auto& key: keys) {
        fn(key);
    }
}

llvm::StringRef CacheStore::base_dir() const {
    return state->base;
}

void CacheStore::State::evict_locked(Namespace& ns, llvm::StringRef keep_key) {
    if(ns.config.policy != CachePolicy::LRU || ns.config.max_bytes == 0 ||
       ns.total_size <= ns.config.max_bytes) {
        return;
    }

    struct Candidate {
        llvm::StringRef key;
        std::int64_t atime;
        std::uint64_t size;
    };

    llvm::SmallVector<Candidate> candidates;
    candidates.reserve(ns.entries.size());
    for(auto& entry: ns.entries) {
        if(entry.first() != keep_key) {
            candidates.push_back({entry.first(), entry.second.atime, entry.second.size});
        }
    }
    std::ranges::sort(candidates, {}, &Candidate::atime);

    for(auto& candidate: candidates) {
        if(ns.total_size <= ns.config.max_bytes) {
            break;
        }
        // A failed delete (e.g. the file is open on Windows) keeps the
        // entry so the next eviction round retries it.
        if(llvm::sys::fs::remove(blob_path(ns, candidate.key))) {
            LOG_DEBUG("CacheStore: cannot evict {} from {}, retrying later",
                      candidate.key,
                      ns.config.name);
            continue;
        }
        LOG_PERF("cache",
                 "ns={} event=evict key={} bytes={}",
                 ns.config.name,
                 candidate.key,
                 candidate.size);
        ns.total_size -= candidate.size;
        ns.entries.erase(candidate.key);
        dirty = true;
    }
}

void CacheStore::State::checkpoint_locked() {
    if(!dirty) {
        return;
    }

    ManifestData data;
    for(auto& [name, ns_state]: namespaces) {
        if(ns_state.config.policy == CachePolicy::Scratch) {
            continue;
        }
        for(auto& entry: ns_state.entries) {
            data.entries.push_back({name.str(), entry.first().str(), entry.second.atime});
        }
    }

    auto json = kota::codec::json::to_json(data);
    if(!json) {
        LOG_WARN("CacheStore: failed to serialize manifest");
        return;
    }

    auto manifest_path = path::join(base, "manifest.json");
    auto tmp_path = path::join(tmp_dir, "manifest.json");
    if(auto result = fs::write(tmp_path, *json); !result) {
        LOG_WARN("CacheStore: failed to write manifest: {}", result.error().message());
        return;
    }
    if(auto result = fs::rename(tmp_path, manifest_path); !result) {
        LOG_WARN("CacheStore: failed to publish manifest: {}", result.error().message());
        return;
    }

    dirty = false;
    changes_since_checkpoint = 0;
}

void CacheStore::checkpoint() {
    std::lock_guard guard(state->mutex);
    state->checkpoint_locked();
}

void CacheStore::maybe_checkpoint() {
    std::lock_guard guard(state->mutex);
    if(state->changes_since_checkpoint >= checkpoint_interval) {
        state->checkpoint_locked();
    }
}

void CacheStore::shutdown() {
    std::lock_guard guard(state->mutex);
    state->checkpoint_locked();

    fs::remove_all(state->tmp_dir);
    for(auto& [name, ns_state]: state->namespaces) {
        if(ns_state.config.policy == CachePolicy::Scratch) {
            fs::remove_all(ns_state.dir);
        }
    }
}

}  // namespace clice
