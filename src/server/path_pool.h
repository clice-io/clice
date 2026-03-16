#pragma once

#include <cassert>
#include <cstdint>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

/// Global path interning pool for the server process.
/// Maps file paths to stable uint32_t IDs, avoiding repeated string storage
/// across DocumentState, CompileGraph, FuzzyGraph, and index queries.
struct ServerPathPool {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> paths;
    llvm::StringMap<std::uint32_t> cache;

    std::uint32_t intern(llvm::StringRef path) {
        assert(!path.empty());
        auto [it, inserted] = cache.try_emplace(path, static_cast<std::uint32_t>(paths.size()));
        if(inserted) {
            auto data = allocator.Allocate<char>(path.size() + 1);
            std::copy(path.begin(), path.end(), data);
            data[path.size()] = '\0';
            auto saved = llvm::StringRef(data, path.size());
            paths.push_back(saved);
            it->second = static_cast<std::uint32_t>(paths.size() - 1);
        }
        return it->second;
    }

    llvm::StringRef resolve(std::uint32_t id) const {
        assert(id < paths.size());
        return paths[id];
    }

    std::uint32_t size() const {
        return static_cast<std::uint32_t>(paths.size());
    }

    bool contains(llvm::StringRef path) const {
        return cache.count(path) > 0;
    }
};

}  // namespace clice
