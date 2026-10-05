#include "vfs/file_table.h"

#include <algorithm>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// `path` with its `..` segments resolved as text.
static Spelling fold(const Spelling& path) {
    llvm::SmallString<256> text(path.str());
    path::remove_dots(text, /*remove_dot_dot=*/true);
    return Spelling::absolute(text);
}

Fid FileTable::intern(const Spelling& path) {
    if(auto it = ids.find(path.str()); it != ids.end()) {
        return it->second;
    }
    auto fid = intern(CanonicalPath(path));
    ids.try_emplace(path.str(), fid);
    return fid;
}

Fid FileTable::intern_spelled(const Spelling& path) {
    auto fid = intern(path);
    spell_as(fid, path);
    return fid;
}

Fid FileTable::intern(CanonicalRef identity) {
    auto [it, inserted] =
        ids.try_emplace(identity, Fid{static_cast<std::uint32_t>(spellings.size())});
    if(inserted) {
        // Allocate with null terminator so that resolve().data() is safe
        // to use as const char* (e.g. in MemoryBuffer::getFile which calls strlen).
        const std::size_t n = identity.size();
        char* buf = allocator.Allocate<char>(n + 1);
        std::ranges::copy(llvm::StringRef(identity), buf);
        buf[n] = '\0';
        spellings.push_back(llvm::StringRef(buf, n));
    }
    return it->second;
}

std::optional<Fid> FileTable::find(const Spelling& path) const {
    auto it = ids.find(path.str());
    if(it == ids.end()) {
        it = ids.find(CanonicalPath(path));
    }
    if(it == ids.end()) {
        return std::nullopt;
    }
    return it->second;
}

void FileTable::spell_as(Fid fid, const Spelling& path) {
    if(llvm::StringRef(path) != llvm::StringRef(resolve(fid))) {
        spelled.try_emplace(fid, path.str());
    }
}

Spelling FileTable::spelling(Fid fid) const {
    if(auto it = spelled.find(fid); it != spelled.end()) {
        return Spelling::absolute(it->second);
    }
    return Spelling(resolve(fid));
}

std::string FileTable::display(Fid fid) const {
    if(auto it = shown.find(fid); it != shown.end()) {
        return it->second;
    }
    return display(resolve(fid));
}

std::string FileTable::display(CanonicalRef identity) const {
    for(auto& [real, root]: spelled_roots) {
        if(path::under(identity, real)) {
            auto rest = llvm::StringRef(identity).drop_front(real.size()).ltrim('/');
            return Spelling(rest, root).str();
        }
    }
    return identity.str();
}

void FileTable::spell_root(const Spelling& root) {
    CanonicalPath real(root);
    auto folded = fold(root);
    if(folded.str() != real.str() && CanonicalPath(folded) == real) {
        spelled_roots.emplace_back(std::move(real), std::move(folded));
    }
}

void FileTable::unspell_root(const Spelling& root) {
    auto folded = fold(root);
    llvm::erase_if(spelled_roots, [&](auto& entry) { return entry.second == folded; });
}

VersionID FileTable::intern_version(Fid fid, std::uint64_t content_hash) {
    auto [it, inserted] =
        version_ids.try_emplace({fid, content_hash},
                                VersionID{static_cast<std::uint32_t>(versions.size())});
    if(inserted) {
        versions.push_back(FileVersion{.fid = fid, .content_hash = content_hash});
    }
    return it->second;
}

vfs::DiskState::Verdict FileTable::check_version(VersionID vid) {
    auto& version = this->version(vid);
    return disk.check(version.fid, version.content_hash);
}

}  // namespace clice
