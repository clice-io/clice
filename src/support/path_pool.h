#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

/// Canonical spelling for path identity: forward slashes and a lowercase
/// drive letter. Lowercase is not a free choice — vscode-uri and every
/// LSP client key documents by lowercase-drive URIs, so pool-resolved
/// strings must be directly URI-emittable in that form; uppercase URIs
/// from the server would never match a client's document identity.
inline std::string canonicalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if(path.size() >= 2 && path[1] == ':' && llvm::isUpper(path[0])) {
        path[0] = llvm::toLower(path[0]);
    }
    return path;
}

/// Intern pool that maps file paths to compact uint32_t IDs.
///
/// Paths are opaque byte strings; the normalization applied is the
/// canonical spelling above, so the URI form VS Code sends
/// ("file:///f%3A/...") and the "F:/..." form the CDB and clang report
/// intern to one ID. Without it every CDB lookup on Windows missed and
/// compiles fell back to guessed commands.
///
/// FIXME: other path components keep their case, so case-variant
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

    /// True for "X:..." drive prefixes with an uppercase letter.
    static bool has_upper_drive(llvm::StringRef path) {
        return path.size() >= 2 && path[1] == ':' && llvm::isUpper(path[0]);
    }

    std::uint32_t intern(llvm::StringRef path) {
        // Apply the canonical spelling so that paths from different sources
        // (URI decoding, CDB, include resolution) compare equal on Windows.
        llvm::SmallString<256> normalized;
        bool needs_normalize = path.contains('\\') || has_upper_drive(path);
        if(needs_normalize) {
            normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            if(has_upper_drive(normalized)) {
                normalized[0] = llvm::toLower(normalized[0]);
            }
            path = normalized;
        }

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
        llvm::SmallString<256> normalized;
        if(path.contains('\\') || has_upper_drive(path)) {
            normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            if(has_upper_drive(normalized)) {
                normalized[0] = llvm::toLower(normalized[0]);
            }
            path = normalized;
        }
        auto it = cache.find(path);
        if(it == cache.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

}  // namespace clice
