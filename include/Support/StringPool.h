#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Allocator.h"

namespace clice {

/// A simple string pool to hold all cstring and cstring list.
/// The lifetime of returned cstring and cstring list is
/// managed by StringPool object.
class StringPool {
public:
    /// Check whether the string is already in the pool.
    bool contains(llvm::StringRef str) const {
        return pooled_strs.contains(str);
    }

    /// Check whether the cstring list is already in the pool.
    bool contains(llvm::ArrayRef<const char*> list) const {
        return pooled_str_lists.contains(list);
    }

    /// Save a cstring in the pool, make sure end with `\0`.
    auto save_cstr(llvm::StringRef str) -> llvm::StringRef;

    /// Save a cstring list in the pool.
    auto save_cstr_list(llvm::ArrayRef<const char*> list) -> llvm::ArrayRef<const char*>;

    /// Clear all cached strings. This causes all previously returned
    /// cstrings and cstring lists to be invalid.
    void clear() {
        pooled_strs.clear();
        pooled_str_lists.clear();
        allocator.Reset();
    }

    /// Get the total bytes allocated in the pool.
    size_t bytes_allocated() const {
        return allocator.getBytesAllocated();
    }

    const llvm::DenseSet<llvm::StringRef>& get_pooled_strs() const {
        return pooled_strs;
    }

    const llvm::DenseSet<llvm::ArrayRef<const char*>>& get_pooled_str_lists() const {
        return pooled_str_lists;
    }

    /// Get the underlying allocator.
    llvm::BumpPtrAllocator& get_allocator() {
        return allocator;
    }

private:
    /// The memory pool to hold all cstring and cstring list.
    llvm::BumpPtrAllocator allocator;

    /// Cache between input string and its cache cstring in the allocator, make sure end with `\0`.
    llvm::DenseSet<llvm::StringRef> pooled_strs;

    /// Cache between input command and its cache array in the allocator.
    llvm::DenseSet<llvm::ArrayRef<const char*>> pooled_str_lists;
};

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<llvm::ArrayRef<const char*>> {
    using T = llvm::ArrayRef<const char*>;

    inline static T getEmptyKey() {
        return T(reinterpret_cast<T::const_pointer>(~0), T::size_type(0));
    }

    inline static T getTombstoneKey() {
        return T(reinterpret_cast<T::const_pointer>(~1), T::size_type(0));
    }

    static unsigned getHashValue(const T& value) {
        return llvm::hash_combine_range(value.begin(), value.end());
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm
