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

/// The master-side table of every file the workspace touches: a path
/// spelling is interned once to a compact fid, and downstream code
/// references files by fid. A fid names a spelling, not an on-disk
/// file — case variants or links to one file are distinct fids.
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
struct FileTable {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> spellings;
    llvm::StringMap<std::uint32_t> ids;

    std::uint32_t intern(llvm::StringRef path) {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);

        auto [it, inserted] = ids.try_emplace(path, spellings.size());
        if(inserted) {
            // Allocate with null terminator so that resolve().data() is safe
            // to use as const char* (e.g. in MemoryBuffer::getFile which calls strlen).
            const std::size_t n = path.size();
            char* buf = allocator.Allocate<char>(n + 1);
            std::copy(path.begin(), path.end(), buf);
            buf[n] = '\0';
            spellings.push_back(llvm::StringRef(buf, n));
        }
        return it->second;
    }

    llvm::StringRef resolve(std::uint32_t fid) const {
        assert(fid < spellings.size());
        return spellings[fid];
    }

    /// Look up a path without interning it, applying the same
    /// normalization as intern().
    std::optional<std::uint32_t> find(llvm::StringRef path) const {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);
        auto it = ids.find(path);
        if(it == ids.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

}  // namespace clice
