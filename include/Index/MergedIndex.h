#pragma once

#include "TUIndex.h"

#include "llvm/Support/Allocator.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice::index {

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

    /// Whether this index needs rebuilding.
    bool need_update(this const Self& self, llvm::ArrayRef<llvm::StringRef> path_mapping);

    bool need_rewrite() {
        return impl != nullptr;
    }

    /// Remove the index of specific path id.
    void remove(this Self& self, std::uint32_t path_id);

    /// Merge the index with given compilation context.
    void merge(this Self& self,
               std::uint32_t path_id,
               std::chrono::milliseconds build_at,
               std::vector<IncludeLocation> include_locations,
               FileIndex& index);

    /// Merge the index with given header context.
    void merge(this Self& self, std::uint32_t path_id, std::uint32_t include_id, FileIndex& index);

    friend bool operator==(MergedIndex& lhs, MergedIndex& rhs);

private:
    /// The binary serialization data of index. If you load merged index
    /// from disk, we use directly access the data without deserialization
    /// unless you want to modify it.
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    /// The in memory data of the index.
    std::unique_ptr<Impl> impl;
};

}  // namespace clice::index
