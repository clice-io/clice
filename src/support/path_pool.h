#pragma once

#include <cassert>
#include <cstdint>
#include <optional>

#include "support/filesystem.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

/// Intern pool that maps file paths to compact uint32_t IDs.
///
/// Paths are opaque byte strings interned in the canonical spelling of
/// path::canonical, so on Windows the URI form VS Code sends
/// ("file:///f%3A/...") and the "F:/..." form the CDB and clang report
/// intern to one ID — without that, every CDB lookup missed and compiles
/// fell back to guessed commands. POSIX paths are never rewritten.
///
/// FIXME: non-drive components keep their case, so case-variant
/// spellings of one file on a case-insensitive filesystem can still
/// intern to different IDs.
///
/// FIXME: paths are assumed to be valid UTF-8. POSIX filenames
/// are raw bytes; a non-UTF-8 path survives interning but breaks
/// downstream where it is embedded into JSON (worker IPC, the agentic
/// protocol) or percent-decoded by clients that interpret URIs as UTF-8.
struct PathPool {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> paths;
    llvm::StringMap<std::uint32_t> cache;

    std::uint32_t intern(llvm::StringRef path) {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);

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
        assert(id < paths.size());
        return paths[id];
    }

    /// Look up a path without interning it, applying the same
    /// normalization as intern().
    std::optional<std::uint32_t> find(llvm::StringRef path) const {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);
        auto it = cache.find(path);
        if(it == cache.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

}  // namespace clice
