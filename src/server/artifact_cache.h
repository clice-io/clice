#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "support/path_pool.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Content-addressed key for compilation artifacts (PCH/PCM).
/// Computed from the artifact's inputs: source content, compile flags,
/// and keys of dependent artifacts.
using ArtifactKey = std::uint64_t;

/// Two-layer staleness snapshot for compilation artifacts.
///
/// Layer 1 (fast): compare each file's current mtime against build_at.
///   If all mtimes <= build_at, the artifact is fresh (zero I/O beyond stat).
///
/// Layer 2 (precise): for files whose mtime changed, re-hash their content
///   and compare against the stored hash.  If the hash matches, the file was
///   "touched" but not actually modified — skip the rebuild.
struct DepsSnapshot {
    llvm::SmallVector<std::uint32_t> path_ids;
    llvm::SmallVector<std::uint64_t> hashes;
    std::int64_t build_at = 0;
};

/// A single entry in the artifact cache.
struct ArtifactEntry {
    /// Path to the artifact file on disk (.pch or .pcm).
    std::string path;

    /// Keys of artifacts this one depends on (parent PCH or dep PCMs).
    /// For current (non-chained) PCH this is empty.
    /// For PCM it contains keys of imported module PCMs.
    /// Future chained PCH will have exactly one input (the parent chain link).
    llvm::SmallVector<ArtifactKey> inputs;

    /// Source file staleness snapshot (header/source dependencies).
    DepsSnapshot deps;

    /// Last time this entry was accessed (for LRU eviction).
    std::chrono::steady_clock::time_point last_access;
};

/// Content-addressed cache for compilation artifacts (PCH and PCM).
///
/// This is the "cold" storage layer.  It owns artifact files on disk and
/// their staleness metadata.  It does NOT track in-flight builds or which
/// sessions are using which artifacts — that is the "active state" layer
/// managed by Workspace (PCHState, pcm_active).
///
/// Keyed by ArtifactKey = hash(source_content, compile_flags, input_keys).
/// Multiple files with the same preamble and flags share one PCH entry.
///
/// Designed to support future chained PCH (linear input chain) and
/// PCM DAGs (multiple inputs) via the `inputs` field.
class ArtifactCache {
public:
    /// Look up an artifact by key.  Returns nullptr if not found.
    /// Updates last_access on hit.
    ArtifactEntry* lookup(ArtifactKey key);

    /// Insert or replace an artifact entry.
    void insert(ArtifactKey key, ArtifactEntry entry);

    /// Remove a single entry by key.
    void erase(ArtifactKey key);

    /// Invalidate an artifact and all entries that (transitively) depend on it.
    /// Returns the set of invalidated keys.
    llvm::SmallVector<ArtifactKey> invalidate(ArtifactKey key);

    /// Evict least-recently-used entries until at most `max_entries` remain.
    /// Respects dependency ordering: never evicts an entry that is still
    /// referenced as an input by a surviving entry.
    void evict(std::size_t max_entries);

    /// Check if an artifact's source dependencies have changed on disk.
    bool deps_changed(const PathPool& pool, ArtifactKey key) const;

    /// Number of entries in the cache.
    std::size_t size() const {
        return entries.size();
    }

    bool contains(ArtifactKey key) const {
        return entries.count(key);
    }

    /// Persist cache metadata to a JSON file.
    void save(const PathPool& pool, llvm::StringRef cache_dir) const;

    /// Load cache metadata from a JSON file.
    void load(PathPool& pool, llvm::StringRef cache_dir);

    /// Remove artifact files older than max_age_days from disk.
    static void cleanup(llvm::StringRef cache_dir, int max_age_days = 7);

    /// Compute an artifact key from source content and compile flags.
    /// Excludes the source file path to allow cross-file sharing (for PCH).
    static ArtifactKey compute_key(llvm::StringRef content,
                                   llvm::ArrayRef<std::string> arguments,
                                   llvm::ArrayRef<ArtifactKey> input_keys = {});

    /// Overload accepting raw const char* flags (from ResolvedFlags).
    static ArtifactKey compute_key(llvm::StringRef content,
                                   llvm::ArrayRef<const char*> flags,
                                   llvm::ArrayRef<ArtifactKey> input_keys = {});

    /// Compute an artifact key that includes the source file path
    /// (for PCM, where identity matters).
    static ArtifactKey compute_key_with_path(llvm::StringRef path,
                                             llvm::ArrayRef<std::string> arguments,
                                             llvm::ArrayRef<ArtifactKey> input_keys = {});

    /// Overload accepting raw const char* flags (from ResolvedFlags).
    static ArtifactKey compute_key_with_path(llvm::StringRef path,
                                             llvm::ArrayRef<const char*> flags,
                                             llvm::ArrayRef<ArtifactKey> input_keys = {});

private:
    /// Collect all keys that transitively depend on `root`.
    void collect_dependents(ArtifactKey root, llvm::SmallVectorImpl<ArtifactKey>& result) const;

    llvm::DenseMap<ArtifactKey, ArtifactEntry> entries;
};

/// Hash a file's content.  Returns 0 on read failure.
std::uint64_t hash_file(llvm::StringRef path);

/// Capture a deps snapshot from a list of file paths.
DepsSnapshot capture_deps_snapshot(PathPool& pool, llvm::ArrayRef<std::string> deps);

/// Check if any dependency in a snapshot has changed on disk.
bool deps_changed(const PathPool& pool, const DepsSnapshot& snap);

}  // namespace clice
