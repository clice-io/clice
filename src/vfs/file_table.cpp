#include "vfs/file_table.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

/// `path` with its `..` segments resolved as text.
static Spelling fold(const Spelling& path) {
    llvm::SmallString<256> text(path.str());
    path::remove_dots(text, /*remove_dot_dot=*/true);
    return Spelling::absolute(text);
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

}  // namespace clice
