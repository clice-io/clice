#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "index/tu_index.h"

#include "llvm/Support/Allocator.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice::index {

/// A dependency of a compilation context: where it was included and the
/// file's path, interned into the shard's own path table on merge, plus the
/// hash of the bytes the indexing compile consumed for it (0 = unavailable;
/// the staleness baseline then stays conservative for this file).
struct DepLocation {
    llvm::StringRef path;
    std::uint32_t line = 0;
    std::uint32_t include_id = 0;
    std::uint64_t content_hash = 0;
};

class MergedIndex {
private:
    struct Impl;

    using Self = MergedIndex;

    MergedIndex(std::unique_ptr<llvm::MemoryBuffer> buffer, std::unique_ptr<Impl> impl);

    void load_in_memory(this Self& self);

public:
    MergedIndex();

    MergedIndex(llvm::StringRef data);

    MergedIndex(const MergedIndex&) = delete;

    MergedIndex(MergedIndex&& other);

    MergedIndex& operator=(const MergedIndex&) = delete;

    MergedIndex& operator=(MergedIndex&& other);

    ~MergedIndex();

    /// Load merged index from disk
    static MergedIndex load(llvm::StringRef path);

    /// Serialize it to binary format.
    void serialize(this const Self& self, llvm::raw_ostream& out);

    /// Lookup the occurrence in corresponding offset.
    void lookup(this const Self& self,
                std::uint32_t offset,
                llvm::function_ref<bool(const Occurrence&)> callback);

    /// Lookup the relations of given symbol.
    void lookup(this const Self& self,
                SymbolHash symbol,
                RelationKind kind,
                llvm::function_ref<bool(const Relation&)> callback);

    /// Whether this index needs rebuilding. Dependency paths come from the
    /// shard's own path table; shards are fully self-contained.
    bool need_update(this const Self& self);

    bool need_rewrite() {
        return impl != nullptr;
    }

    /// Mutation stamp, reassigned by every merge/remove from a process-wide
    /// monotonic source (values never repeat across objects, so an erase +
    /// re-create at the same key cannot alias an older snapshot). 0 = not
    /// mutated since construction. save() snapshots it before serializing
    /// and flips the shard back to its committed blob only if no mutation
    /// landed across the commit await — otherwise the flip would silently
    /// drop the newer contribution.
    std::uint64_t revision() const {
        return rev;
    }

    /// Whether this index holds any data (a rejected or missing blob loads
    /// as an empty index).
    bool loaded() const {
        return buffer != nullptr || impl != nullptr;
    }

    /// Remove the contribution keyed by `context_path` (a TU for header
    /// shards, the file itself for compilation shards).
    void remove(this Self& self, llvm::StringRef context_path);

    /// Whether this shard holds a contribution keyed by `context_path`.
    /// Cheap on serialized shards: scans the small context tables without
    /// deserializing the shard.
    bool has_contribution(this const Self& self, llvm::StringRef context_path);

    /// Get the stored source content for position mapping.
    llvm::StringRef content(this const Self& self);

    /// Get line starts for position mapping.
    std::span<const std::uint32_t> line_starts(this const Self& self);

    /// Look up a symbol in this shard's local symbol table.
    bool find_symbol(this const Self& self, SymbolHash hash, std::string& name, SymbolKind& kind);

    /// Add symbols to this shard's local symbol table (idempotent by hash).
    void merge_symbols(this Self& self, const SymbolTable& symbols);

    /// Merge the index with given compilation context, keyed by `tu_path`.
    /// Dependency paths are interned into the shard's own table and a content
    /// hash is captured per distinct dependency for the staleness check.
    void merge(this Self& self,
               llvm::StringRef tu_path,
               std::chrono::milliseconds build_at,
               llvm::ArrayRef<DepLocation> deps,
               FileIndex& index,
               llvm::StringRef content);

    /// Merge the index with given header context. @param tu_path is the
    /// including TU: a later merge with the same TU replaces that TU's
    /// previous contribution, other TUs' contributions are untouched.
    void merge(this Self& self,
               llvm::StringRef tu_path,
               std::uint32_t include_id,
               FileIndex& index,
               llvm::StringRef content);

    friend bool operator==(MergedIndex& lhs, MergedIndex& rhs);

private:
    /// The binary serialization data of index. If you load merged index
    /// from disk, we use directly access the data without deserialization
    /// unless you want to modify it.
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    /// The in memory data of the index.
    std::unique_ptr<Impl> impl;

    /// See revision().
    std::uint64_t rev = 0;
};

}  // namespace clice::index
