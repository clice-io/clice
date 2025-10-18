#pragma once

#include "TUIndex.h"

namespace clice::index {

struct PathPool {
    llvm::BumpPtrAllocator allocator;

    std::vector<llvm::StringRef> paths;

    llvm::DenseMap<llvm::StringRef, std::uint32_t> cache;

    llvm::StringRef save(llvm::StringRef s) {
        auto data = allocator.Allocate<char>(s.size() + 1);
        std::ranges::copy(s, data);
        data[s.size()] = '\0';
        return llvm::StringRef(data, s.size());
    }

    auto path_id(llvm::StringRef path) {
        assert(!path.empty());
        auto [it, success] = cache.try_emplace(path, paths.size());
        if(!success) {
            return it->second;
        }

        auto& [k, v] = *it;
        k = save(path);
        paths.emplace_back(k);
        return it->second;
    }

    llvm::StringRef path(std::uint32_t id) {
        return paths[id];
    }
};

struct FileInfo {
    std::int64_t mtime;
};

struct ProjectIndex {
    PathPool path_pool;

    llvm::DenseMap<std::uint32_t, std::uint32_t> indices;

    SymbolTable symbols;

    llvm::SmallVector<std::uint32_t> merge(this ProjectIndex& self, TUIndex& index);

    void serialize(this ProjectIndex& self, llvm::raw_ostream& os);

    static ProjectIndex from(const void* data);
};

}  // namespace clice::index
