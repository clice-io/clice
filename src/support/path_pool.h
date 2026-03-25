#pragma once

#include <algorithm>
#include <cstdint>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

/// Intern pool that maps file paths to compact uint32_t IDs.
struct PathPool {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> paths;
    llvm::StringMap<std::uint32_t> cache;

    std::uint32_t intern(llvm::StringRef path) {
        auto [it, inserted] = cache.try_emplace(path, paths.size());
        if(inserted) {
            // Allocate with null terminator so that resolve().data() is safe
            // to use as const char* (e.g. in MemoryBuffer::getFile which calls strlen).
            const std::size_t n = path.size();
            char* buf = allocator.Allocate<char>(n + 1);
            std::copy(path.begin(), path.end(), buf);
            buf[n] = '\0';
            paths.push_back(llvm::StringRef(buf, n));
        }
        return it->second;
    }

    llvm::StringRef resolve(std::uint32_t id) const {
        return paths[id];
    }
};

}  // namespace clice
