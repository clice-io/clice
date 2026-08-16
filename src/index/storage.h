#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

class CacheStore;

}

namespace clice::index {

/// The three blob families the index persists.
enum class IndexBlobKind : std::uint8_t {
    /// Per-file row blobs (ShardBlob), keyed by a path hash.
    Shard,
    /// Per-TU manifests, keyed by a path hash.
    Manifest,
    /// The single global blob (FileVersion table + symbols), key "global".
    Global,
};

/// Storage backend for index blobs. The filesystem implementation below is
/// the default; a database-backed one plugs in behind the same interface.
///
/// All methods are thread-safe. `write` does the heavy IO (fsync) and
/// belongs off the event loop; reads are cheap.
class IndexStorage {
public:
    virtual ~IndexStorage() = default;

    /// The blob's bytes (memory-mapped where the backend allows), or
    /// nullptr when missing or unreadable.
    virtual std::unique_ptr<llvm::MemoryBuffer> read(IndexBlobKind kind, llvm::StringRef key) = 0;

    struct Blob {
        IndexBlobKind kind;
        std::string key;
        std::string bytes;
    };

    /// Persist a batch in order. Atomicity is per blob, not per batch — a
    /// crash can land a prefix; every load path treats any partially
    /// written combination as stale data to rebuild. An entry that fails
    /// to persist is logged and dropped (the index is rebuildable); the
    /// returned count of durably committed blobs is how callers tell.
    virtual std::size_t write(llvm::ArrayRef<Blob> batch) = 0;

    virtual void remove(IndexBlobKind kind, llvm::StringRef key) = 0;

    virtual void for_each_key(IndexBlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) = 0;
};

/// Filesystem implementation over the cache store; registers the index
/// namespaces on construction.
std::unique_ptr<IndexStorage> make_fs_index_storage(CacheStore& store);

}  // namespace clice::index
