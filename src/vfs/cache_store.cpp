module clice;

import :support.logging;
import :support.process;
import :vfs.cache_store;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// Manifest checkpoint is forced after this many commits/invalidates.
constexpr std::uint32_t checkpoint_interval = 16;

/// Root-level lock file serializing open()'s version removal against layout
/// startup; lives beside the version directories, exempt from the removal.
constexpr llvm::StringLiteral store_lock_name = "store.lock";

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

/// On a rename collision, check whether the existing destination blob is
/// byte-identical to the one we tried to publish.  This path is rare even
/// on Windows: fs::rename already replaces a destination another process
/// holds open with delete sharing (every file kota or LLVM opens), so a
/// collision means a non-cooperating process (AV scanner, indexing
/// service) holds the blob.  The extra reads stay off hot paths.
bool same_content(llvm::StringRef tmp_path, llvm::StringRef final_path) {
    auto tmp_status = fs::status(tmp_path);
    auto final_status = fs::status(final_path);
    if(!tmp_status || !final_status || final_status->type != fs::FileType::Regular ||
       tmp_status->size != final_status->size) {
        return false;
    }

    auto map = [](llvm::StringRef path) {
        return fs::File::open(path).and_then(fs::Mapping::map);
    };
    auto tmp_bytes = map(tmp_path);
    auto final_bytes = map(final_path);
    if(!tmp_bytes || !final_bytes) {
        return false;
    }
    return tmp_bytes->bytes() == final_bytes->bytes();
}

/// Whether a live process is working inside this cache layout: every
/// writable open creates `tmp/{pid}` and keeps it until shutdown.
bool has_live_instance(llvm::StringRef base) {
    auto entries = fs::Directory::open(path::join(base, "tmp"));
    return entries && ranges::any_of(*entries, [](const auto& entry) {
               int pid = 0;
               return entry && !llvm::StringRef(entry->name).getAsInteger(10, pid) &&
                      sys::process_alive(pid);
           });
}

std::int64_t now_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace

struct CacheStore::State {
    struct Entry {
        /// Primary blob size.  total_size accounts size + aux_size.
        std::uint64_t size = 0;
        /// Aux blob size; non-zero means the aux blob is committed (blobs
        /// are never empty, so zero doubles as "absent").
        std::uint64_t aux_size = 0;
        std::int64_t atime = 0;
    };

    struct Namespace {
        CacheNamespace config;
        /// Directory holding this namespace's blobs: `{base}/{name}`.
        std::string dir;
        llvm::StringMap<Entry> entries;
        std::uint64_t total_size = 0;
        /// Keys a commit is publishing outside the lock: whatever lies at
        /// their final paths is the commit's until it finishes.
        llvm::StringSet<> publishing;
    };

    std::mutex mutex;

    /// `{root}/cache/v{version}` — everything the store manages lives here.
    std::string root;
    std::string base;

    /// `{base}/tmp/{pid}` — this instance's in-flight writes.
    std::string tmp_dir;

    llvm::StringMap<Namespace> namespaces;

    /// Last-accessed times loaded from the manifest, keyed by "{ns}/{key}".
    /// Consumed when the corresponding namespace is registered.
    llvm::StringMap<std::int64_t> manifest_atimes;

    std::uint64_t next_tmp_id = 0;

    std::uint32_t changes_since_checkpoint = 0;
    bool dirty = false;

    /// Inspection mode (open's read_only): no directory is created or
    /// removed, and writes are a caller bug.
    bool read_only = false;

    /// Logical clock: strictly increasing per issued generation so that LRU
    /// ordering is deterministic even within one millisecond.
    std::int64_t last_generation = 0;

    std::int64_t next_generation() {
        last_generation = std::max(now_ms(), last_generation + 1);
        return last_generation;
    }

    std::string blob_path(const Namespace& ns, llvm::StringRef key) const {
        return path::join(ns.dir, key.str() + ns.config.extension);
    }

    std::string aux_blob_path(const Namespace& ns, llvm::StringRef key) const {
        return path::join(ns.dir, key.str() + ns.config.aux_extension);
    }

    /// Drop a key's committed aux blob (file + accounting).  Called when
    /// the primary is republished: the old aux describes the old primary,
    /// and an incomplete pair is strictly better than a mismatched one.
    /// The accounting is zeroed even when the delete fails (file open on
    /// Windows) — lookup_aux must stop serving the stale blob immediately;
    /// the leftover file is overwritten by the following aux commit or
    /// removed when the entry is eventually evicted.
    void reset_aux_locked(Namespace& ns, llvm::StringRef key, Entry& entry) {
        if(ns.config.aux_extension.empty() || entry.aux_size == 0) {
            return;
        }
        fs::remove(aux_blob_path(ns, key));
        ns.total_size -= entry.aux_size;
        entry.aux_size = 0;
    }

    Namespace* find_namespace(llvm::StringRef name) {
        auto it = namespaces.find(name);
        return it != namespaces.end() ? &it->second : nullptr;
    }

    void evict_locked(Namespace& ns, llvm::StringRef keep_key);
    void checkpoint_locked();

    /// Evictions recorded for take_evictions(); guarded by mutex.
    std::vector<EvictedBlob> evictions;
};

CacheStore::CacheStore(std::unique_ptr<State> state) : state(std::move(state)) {}

CacheStore::CacheStore(CacheStore&&) noexcept = default;

CacheStore& CacheStore::operator=(CacheStore&&) noexcept = default;

CacheStore::~CacheStore() = default;

std::expected<CacheStore, std::error_code> CacheStore::open(llvm::StringRef root,
                                                            std::uint32_t version,
                                                            bool read_only) {
    assert(!root.empty() && "cache root must not be empty");

    auto state = std::make_unique<State>();
    state->read_only = read_only;

    auto parent = path::join(root, "cache");
    auto version_dir = std::format("v{}", version);
    state->root = std::string(root);
    state->base = path::join(parent, version_dir);

    if(read_only) {
        if(!fs::is_directory(state->base)) {
            return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
        }
        return CacheStore(std::move(state));
    }

    // Only the parent may exist before the lock is held: it holds the lock
    // file and is never removed.
    if(auto made = fs::create_directories(parent); !made) {
        return std::unexpected(made.error());
    }

    // The pid marker created below is what shields a layout from another
    // version's removal pass, but it only exists at the end of this function: a
    // pass scanning between our create_directories and the marker would
    // reclaim the layout we are initializing. This root-level lock covers
    // exactly that window — every writable open holds it from before its
    // version directory exists until its marker does. Best effort: on a
    // filesystem without advisory locks the window stays open, as before.
    auto lock_path = path::join(parent, store_lock_name);
    auto lock = fs::File::open(lock_path, {.read = true, .write = true, .create = true});
    if(!lock) {
        LOG_WARN("CacheStore: cannot open {}: {}", lock_path, lock.error().message());
    } else if(auto error = lock->lock()) {
        LOG_WARN("CacheStore: cannot lock {}: {}", lock_path, error.message());
    }

    if(auto made = fs::create_directories(state->base); !made) {
        return std::unexpected(made.error());
    }

    // Discard anything that isn't the current layout version: older version
    // directories and any stray files. A layout still holding a live
    // instance stays: a clice of another version is serving from it, and
    // its writer locks live inside the directory, so they cannot protect
    // it from us — a later open reclaims it once that process exits.
    if(auto entries = fs::Directory::open(parent)) {
        for(auto& entry: *entries) {
            if(!entry) {
                break;
            }
            if(entry->name == version_dir || entry->name == store_lock_name) {
                continue;
            }
            auto layout = path::join(parent, entry->name);
            if(entry->type != fs::FileType::Directory) {
                LOG_INFO("CacheStore: discarding stale cache layout {}", layout);
                fs::remove(layout);
                continue;
            }
            if(has_live_instance(layout)) {
                LOG_INFO("CacheStore: keeping cache layout {}, still in use", layout);
                continue;
            }
            LOG_INFO("CacheStore: discarding stale cache layout {}", layout);
            fs::remove_all(layout);
        }
    }

    // Load the manifest.  Corrupt or missing is fine: registration falls
    // back to a directory scan with mtimes as last-accessed times.
    auto manifest_path = path::join(state->base, "manifest.json");
    if(std::string content; !fs::read_file(content, manifest_path)) {
        ManifestData data;
        if(json::from_string(content, data)) {
            for(auto& entry: data.entries) {
                auto key = entry.ns + "/" + entry.key;
                state->manifest_atimes[key] = entry.atime;
                state->last_generation = std::max(state->last_generation, entry.atime);
            }
        } else {
            LOG_WARN("CacheStore: corrupt manifest {}, rebuilding from directory scan",
                     manifest_path);
        }
    }

    // The only crash residue are in-flight tmp files; committed blobs are
    // complete by construction (atomic rename).  Remove tmp directories of
    // instances that no longer exist.
    auto self_pid = sys::pid();
    auto tmp_parent = path::join(state->base, "tmp");
    if(auto entries = fs::Directory::open(tmp_parent)) {
        for(auto& entry: *entries) {
            if(!entry) {
                break;
            }
            int pid = 0;
            if(llvm::StringRef(entry->name).getAsInteger(10, pid) || pid == self_pid ||
               sys::process_alive(pid)) {
                continue;
            }
            auto dead = path::join(tmp_parent, entry->name);
            fs::remove_all(dead);
            LOG_DEBUG("CacheStore: removed dead instance directory {}", dead);
        }
    }

    state->tmp_dir = path::join(tmp_parent, std::to_string(self_pid));
    fs::remove_all(state->tmp_dir);
    if(auto made = fs::create_directories(state->tmp_dir); !made) {
        return std::unexpected(made.error());
    }

    return CacheStore(std::move(state));
}

void CacheStore::write_ignore_markers(llvm::StringRef root) {
    // The root ignores itself, so the project-local default never
    // pollutes the repository: `*` covers git and everything honoring
    // gitignore (ripgrep, editor search), except `config.toml` — the root
    // doubles as the `.clice/config.toml` location, and user configuration
    // must stay visible. CACHEDIR.TAG (the standard marker backup and
    // scanning tools skip) sits at the root too: it must cover generated
    // content outside the store (logs), and
    // root-level files are safe from open()'s layout removal, which only
    // scans cache/. The known cost is that CACHEDIR-aware backups also
    // skip a config.toml kept here — git, which sees it, is the intended
    // preservation channel.
    if(auto made = fs::create_directories(root); !made) {
        LOG_WARN("CacheStore: cannot create {}: {}", root, made.error().message());
        return;
    }
    auto write_marker = [](llvm::StringRef path, llvm::StringRef content) {
        // create_new makes creation the existence check, so a marker that
        // appears concurrently is never overwritten either.
        auto file = fs::File::open(path, {.write = true, .create_new = true});
        if(!file) {
            if(file.error() != std::errc::file_exists) {
                LOG_WARN("CacheStore: cannot write {}: {}", path, file.error().message());
            }
            return;
        }
        if(auto error = file->write(content)) {
            LOG_WARN("CacheStore: cannot write {}: {}", path, error.message());
            // create_new proved this call created the file, so removing
            // the partial marker clobbers no concurrent writer and lets a
            // later session retry.
            fs::remove(path);
        }
    };
    write_marker(path::join(root, ".gitignore"), "*\n!config.toml\n");
    write_marker(path::join(root, "CACHEDIR.TAG"),
                 "Signature: 8a477f597d28d172789f06886806bc55\n"
                 "# This file marks a cache directory created by clice.\n"
                 "# For information see https://bford.info/cachedir/\n");
}

void CacheStore::register_namespace(CacheNamespace ns) {
    std::lock_guard guard(state->mutex);

    auto ns_dir = path::join(state->base, ns.name);
    if(!state->read_only) {
        fs::create_directories(ns_dir);
    }

    [[maybe_unused]] auto [it, inserted] = state->namespaces.try_emplace(ns.name);
    assert(inserted && "namespace registered twice");
    auto& ns_state = it->second;
    ns_state.config = std::move(ns);
    ns_state.dir = std::move(ns_dir);

    // Adopt blobs already on disk.  The directory scan, not the manifest,
    // decides existence: this also picks up blobs committed after the last
    // checkpoint of a crashed instance.  Aux blobs are collected aside and
    // attached after the scan — directory order is arbitrary, so an aux
    // file may be seen before its primary.
    struct AuxBlob {
        std::uint64_t size;
        std::int64_t mtime_ns;
    };

    auto listed = fs::Directory::open(ns_state.dir);
    if(!listed && !(state->read_only && listed.error() == std::errc::no_such_file_or_directory)) {
        // A read-only open of a store whose namespace was never created is
        // a legitimately empty scan; any other failure hides existing blobs.
        LOG_WARN("CacheStore: failed to scan namespace {}: {}",
                 ns_state.dir,
                 listed.error().message());
    }
    llvm::StringMap<AuxBlob> aux_blobs;
    llvm::StringMap<std::int64_t> primary_mtimes;
    if(listed) {
        for(auto& entry: *listed) {
            if(!entry) {
                LOG_WARN("CacheStore: failed to scan namespace {}: {}",
                         ns_state.dir,
                         entry.error().message());
                break;
            }
            llvm::StringRef filename = entry->name;

            auto status = entry->status();
            if(!status || status->type != fs::FileType::Regular) {
                continue;
            }
            auto stamp = vfs::Stamp::of(*status);

            auto& aux_ext = ns_state.config.aux_extension;
            if(!aux_ext.empty() && filename.ends_with(aux_ext)) {
                aux_blobs[filename.drop_back(aux_ext.size())] = {stamp.size, stamp.mtime_ns};
                continue;
            }

            auto& ext = ns_state.config.extension;
            if(!ext.empty() && !filename.consume_back(ext)) {
                continue;
            }

            auto atime_it =
                state->manifest_atimes.find(ns_state.config.name + "/" + filename.str());
            auto atime = atime_it != state->manifest_atimes.end() ? atime_it->second
                                                                  : stamp.mtime_ns / 1'000'000;

            ns_state.entries[filename] = {stamp.size, 0, atime};
            ns_state.total_size += stamp.size;
            primary_mtimes[filename] = stamp.mtime_ns;
        }
    }

    // Attach aux blobs to their entries. An aux without a primary is crash
    // residue (a pair evicted halfway); an aux OLDER than its primary is
    // residue of a removal that failed while the file was held open (see
    // reset_aux_locked) — writers put the aux file on disk only after the
    // primary is fully flushed (see handle_build_pch) and pairs commit
    // primary-first, so a legitimate aux is never older, and adopting one
    // would pair a fresh primary with stale aux data. Remove both kinds,
    // nothing references them.
    for(auto& entry: aux_blobs) {
        auto key = entry.getKey();
        auto it = ns_state.entries.find(key);
        if(it != ns_state.entries.end() && entry.getValue().mtime_ns >= primary_mtimes[key]) {
            it->second.aux_size = entry.getValue().size;
            ns_state.total_size += entry.getValue().size;
        } else if(!state->read_only) {
            fs::remove(state->aux_blob_path(ns_state, key));
            LOG_DEBUG("CacheStore: removed stale aux blob {} in {}", key, ns_state.config.name);
        }
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

    it->second.atime = state->next_generation();
    state->dirty = true;
    return state->blob_path(*ns_state, key);
}

std::optional<std::string> CacheStore::lookup_aux(llvm::StringRef ns, llvm::StringRef key) {
    std::lock_guard guard(state->mutex);

    auto* ns_state = state->find_namespace(ns);
    if(!ns_state || ns_state->config.aux_extension.empty()) {
        return std::nullopt;
    }

    auto it = ns_state->entries.find(key);
    if(it == ns_state->entries.end() || it->second.aux_size == 0) {
        return std::nullopt;
    }

    it->second.atime = state->next_generation();
    state->dirty = true;
    return state->aux_blob_path(*ns_state, key);
}

CacheStore::PendingEntry CacheStore::begin_store(llvm::StringRef ns, llvm::StringRef key) {
    std::lock_guard guard(state->mutex);
    return begin_store_locked(ns, key, /*aux=*/false);
}

CacheStore::PendingEntry CacheStore::begin_store_aux(llvm::StringRef ns, llvm::StringRef key) {
    std::lock_guard guard(state->mutex);
    return begin_store_locked(ns, key, /*aux=*/true);
}

CacheStore::PendingEntry CacheStore::begin_store_locked(llvm::StringRef ns,
                                                        llvm::StringRef key,
                                                        bool aux) {
    assert(!state->read_only && "write on a read-only store");
    auto* ns_state = state->find_namespace(ns);
    assert(ns_state && "begin_store on unregistered namespace");
    auto& extension = aux ? ns_state->config.aux_extension : ns_state->config.extension;
    assert(!extension.empty() && "begin_store_aux on a namespace without aux blobs");

    return PendingEntry{ns.str(), key.str(), next_tmp_path(extension), aux};
}

CacheStore::PendingEntry CacheStore::begin_transfer() {
    std::lock_guard guard(state->mutex);
    assert(!state->read_only && "write on a read-only store");
    return PendingEntry{{}, {}, next_tmp_path(".transfer")};
}

std::string CacheStore::next_tmp_path(llvm::StringRef extension) {
    // The cache directory can be wiped externally while the server runs
    // (a user resetting state with `rm -rf`); re-create the tmp dir so
    // writers don't fail forever afterwards. Idempotent and cheap.
    if(auto made = fs::create_directories(state->tmp_dir); !made) {
        LOG_WARN("CacheStore: cannot re-create tmp dir {}: {}",
                 state->tmp_dir,
                 made.error().message());
    }

    auto tmp_id = state->next_tmp_id;
    state->next_tmp_id += 1;
    return path::join(state->tmp_dir, std::format("{}{}", tmp_id, extension));
}

std::expected<std::string, std::error_code> CacheStore::commit(PendingEntry pending) {
    auto status = fs::status(pending.tmp_path);
    if(!status) {
        return std::unexpected(status.error());
    }

    // The file work runs outside the lock, which lookups on the event loop
    // take at every request: the fsync flushes to disk, a rename onto a
    // destination another process holds open retries for up to two seconds
    // on Windows, and telling a benign collision from a stale blob reads
    // both.
    std::string final_path;
    std::string dir;
    {
        std::lock_guard guard(state->mutex);
        auto* ns_state = state->find_namespace(pending.ns);
        assert(ns_state && "commit of an entry begin_store did not hand out");
        // An aux blob attaches to a live entry; without one (primary commit
        // failed, or the entry was evicted in between) it would be an
        // orphan — refuse, the caller rebuilds the pair.
        if(pending.aux && !ns_state->entries.contains(pending.key)) {
            fs::remove(pending.tmp_path);
            return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
        }
        final_path = pending.aux ? state->aux_blob_path(*ns_state, pending.key)
                                 : state->blob_path(*ns_state, pending.key);
        dir = ns_state->dir;
        [[maybe_unused]] bool alone = ns_state->publishing.insert(pending.key).second;
        assert(alone && "one commit per key at a time");
    }

    // The destination was removed for a retry that failed too.
    bool removed = false;
    // A crash cannot leave a blob whose name promises content it doesn't
    // have.
    auto failure = [&] {
        auto file = fs::File::open(pending.tmp_path);
        return file ? file->sync() : file.error();
    }();
    if(failure) {
        fs::remove(pending.tmp_path);
    } else {
        // The namespace dir can be wiped externally while the server runs;
        // re-create it so the rename below doesn't fail forever.
        if(auto made = fs::create_directories(dir); !made) {
            LOG_WARN("CacheStore: cannot re-create dir {}: {}", dir, made.error().message());
        }
        if(auto error = fs::rename(pending.tmp_path, final_path)) {
            if(same_content(pending.tmp_path, final_path)) {
                // Benign collision: an identical blob is already published
                // (Windows, destination currently open).  Keep the survivor
                // and account for it.  This is verified by comparison, not
                // assumed from the key: keys are not fully content-addressed
                // (a dependency edit changes the PCH content without
                // changing its key input).
                fs::remove(pending.tmp_path);
                status = fs::status(final_path);
                if(!status) {
                    failure = error;
                }
            } else {
                // The destination is stale — a blob whose content drifted
                // from its key.  Remove it and retry; if the rename still fails, report the error
                // instead of silently dropping the new data.
                fs::remove(final_path);
                if(auto retry = fs::rename(pending.tmp_path, final_path)) {
                    fs::remove(pending.tmp_path);
                    failure = retry;
                    removed = true;
                }
            }
        }
    }
    if(!failure) {
        auto directory = fs::Directory::open(dir);
        if(auto error = directory ? directory->sync() : directory.error()) {
            LOG_WARN("CacheStore: cannot sync dir {}: {}", dir, error.message());
        }
    }
    bool gone = removed && !fs::exists(final_path);

    {
        std::lock_guard guard(state->mutex);
        auto* ns_state = state->find_namespace(pending.ns);
        ns_state->publishing.erase(pending.key);
        auto it = ns_state->entries.find(pending.key);
        bool entry_alive = it != ns_state->entries.end();
        assert((entry_alive || !pending.aux) && "evicting or invalidating a key mid-commit");
        if(failure) {
            if(removed && pending.aux) {
                // The removal above may have deleted a committed aux blob;
                // stop serving it. The primary is intact, the pair is
                // merely incomplete.
                ns_state->total_size -= it->second.aux_size;
                it->second.aux_size = 0;
                state->dirty = true;
            } else if(gone && !pending.aux && entry_alive) {
                // The old blob is gone as well: drop its entry so lookups
                // don't hand out a dangling path.
                state->reset_aux_locked(*ns_state, pending.key, it->second);
                ns_state->total_size -= it->second.size;
                ns_state->entries.erase(it);
                state->dirty = true;
            }
            return std::unexpected(failure);
        }

        auto& entry = ns_state->entries[pending.key];
        if(pending.aux) {
            ns_state->total_size += status->size - entry.aux_size;
            entry.aux_size = status->size;
        } else {
            // A republished primary invalidates the old aux blob: serving
            // yesterday's aux next to today's primary would be a silent
            // mismatch, while an incomplete pair is a plain cache miss.
            state->reset_aux_locked(*ns_state, pending.key, entry);
            // Unsigned wraparound is intentional and exact here: entry.size
            // is already included in total_size, so total + new - old stays
            // correct even when the replacement blob is smaller.
            ns_state->total_size += status->size - entry.size;
            entry.size = status->size;
        }
        entry.atime = state->next_generation();

        state->evict_locked(*ns_state, pending.key);
        state->dirty = true;
        state->changes_since_checkpoint += 1;
    }

    maybe_checkpoint();
    return final_path;
}

void CacheStore::PendingEntry::remove_tmp() {
    if(!tmp_path.empty()) {
        fs::remove(tmp_path);
    }
}

void CacheStore::invalidate(llvm::StringRef ns, llvm::StringRef key) {
    {
        std::lock_guard guard(state->mutex);
        if(state->read_only) {
            return;
        }

        auto* ns_state = state->find_namespace(ns);
        if(!ns_state) {
            return;
        }

        auto it = ns_state->entries.find(key);
        if(it == ns_state->entries.end()) {
            return;
        }
        assert(!ns_state->publishing.contains(key) && "invalidating a key mid-commit");

        // FIXME: the entry goes whether or not the removes succeed: on a volume
        // without POSIX deletes (FAT) a blob a worker maps stays behind,
        // orphaned; eviction keeps such an entry for a retry.
        if(!ns_state->config.aux_extension.empty()) {
            fs::remove(state->aux_blob_path(*ns_state, key));
        }
        fs::remove(state->blob_path(*ns_state, key));
        ns_state->total_size -= it->second.size + it->second.aux_size;
        ns_state->entries.erase(it);

        state->dirty = true;
        state->changes_since_checkpoint += 1;
    }

    maybe_checkpoint();
}

std::vector<CacheStore::EvictedBlob> CacheStore::take_evictions() {
    std::lock_guard guard(state->mutex);
    return std::exchange(state->evictions, {});
}

std::size_t CacheStore::pending_tmp_files() const {
    std::lock_guard guard(state->mutex);
    auto entries = fs::Directory::open(state->tmp_dir);
    return entries ? ranges::count_if(*entries, [](const auto& entry) { return entry.has_value(); })
                   : 0;
}

llvm::StringRef CacheStore::base_dir() const {
    return state->base;
}

llvm::StringRef CacheStore::root_dir() const {
    return state->root;
}

bool CacheStore::read_only() const {
    return state->read_only;
}

void CacheStore::State::evict_locked(Namespace& ns, llvm::StringRef keep_key) {
    if(read_only || ns.config.max_bytes == 0 || ns.total_size <= ns.config.max_bytes) {
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
        if(entry.first() != keep_key && !ns.publishing.contains(entry.first())) {
            candidates.push_back(
                {entry.first(), entry.second.atime, entry.second.size + entry.second.aux_size});
        }
    }
    ranges::sort(candidates, {}, &Candidate::atime);

    for(auto& candidate: candidates) {
        if(ns.total_size <= ns.config.max_bytes) {
            break;
        }
        // Aux first, then primary: whatever a failure leaves behind is a
        // served primary with a missing aux (a plain pair miss), never an
        // orphan aux.  A failed delete (e.g. the file is open on Windows)
        // keeps the entry so the next eviction round retries it.
        if(!ns.config.aux_extension.empty()) {
            auto it = ns.entries.find(candidate.key);
            if(it->second.aux_size != 0) {
                if(!fs::remove(aux_blob_path(ns, candidate.key))) {
                    LOG_DEBUG("CacheStore: cannot evict aux {} from {}, retrying later",
                              candidate.key,
                              ns.config.name);
                    continue;
                }
                ns.total_size -= it->second.aux_size;
                it->second.aux_size = 0;
            }
        }
        if(!fs::remove(blob_path(ns, candidate.key))) {
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
        auto it = ns.entries.find(candidate.key);
        ns.total_size -= it->second.size;
        // Record before the erase: the candidate's key view borrows the
        // entry's own storage.
        evictions.push_back({ns.config.name, candidate.key.str()});
        ns.entries.erase(it);
        dirty = true;
    }
}

void CacheStore::State::checkpoint_locked() {
    // Read-only: lookups still bump in-memory atimes (setting dirty), but
    // nothing may reach the disk — with no tmp_dir the write below would
    // even land a manifest in the process working directory.
    if(read_only || !dirty) {
        return;
    }

    ManifestData data;
    for(auto& [name, ns_state]: namespaces) {
        for(auto& entry: ns_state.entries) {
            data.entries.push_back({name.str(), entry.first().str(), entry.second.atime});
        }
    }

    auto json = json::to_string(data);
    if(!json) {
        LOG_WARN("CacheStore: failed to serialize manifest");
        return;
    }

    auto manifest_path = path::join(base, "manifest.json");
    auto tmp_path = path::join(tmp_dir, "manifest.json");
    if(auto error = fs::write_file(tmp_path, *json)) {
        LOG_WARN("CacheStore: failed to write manifest: {}", error.message());
        return;
    }
    if(auto error = fs::rename(tmp_path, manifest_path)) {
        LOG_WARN("CacheStore: failed to publish manifest: {}", error.message());
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
    if(state->read_only) {
        return;
    }
    state->checkpoint_locked();

    fs::remove_all(state->tmp_dir);
}

}  // namespace clice
