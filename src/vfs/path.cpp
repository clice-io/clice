module;

#include "support/prelude.h"

module clice;

import :vfs.path;

namespace clice {

/// `.` segments, duplicate and trailing separators dropped, canonically
/// spelled; `..` kept.
static std::string normalized(std::string text) {
    path::normalize(text, path::DotDot::Keep);
    path::canonicalize(text);
    return text;
}

llvm::StringRef path::portable(llvm::StringRef p,
                               llvm::StringRef workspace,
                               llvm::SmallVectorImpl<char>& storage) {
    if(workspace.empty() || !is_within(p, workspace)) {
        return p;
    }
    auto rest = p.drop_front(workspace.size());
    storage.assign(workspace_anchor.begin(), workspace_anchor.end());
    if(!rest.empty() && !is_separator(rest.front())) {
        storage.push_back('/');
    }
    storage.append(rest.begin(), rest.end());
    return llvm::StringRef(storage.data(), storage.size());
}

llvm::StringRef path::local(llvm::StringRef name,
                            llvm::StringRef workspace,
                            llvm::SmallVectorImpl<char>& storage) {
    if(!name.consume_front(workspace_anchor)) {
        return name;
    }
    assert(!workspace.empty() && "a portable name read without its workspace");
    storage.assign(workspace.begin(), workspace.end());
    if(workspace.ends_with("/")) {
        name.consume_front("/");
    }
    storage.append(name.begin(), name.end());
    return llvm::StringRef(storage.data(), storage.size());
}

Spelling::Spelling(llvm::StringRef text, const Spelling& base) {
    std::string joined = base.text;
    path::push(joined, text);
    this->text = normalized(std::move(joined));
}

Spelling Spelling::absolute(llvm::StringRef text) {
    assert(path::is_absolute(text) && "an unanchored path");
    Spelling result;
    result.text = normalized(text.str());
    return result;
}

Spelling Spelling::cwd() {
    std::string directory;
    fs::current_dir(directory);
    return absolute(directory);
}

Spelling::Spelling(CanonicalRef identity) : text(identity.str()) {}

Spelling Spelling::parent() const {
    Spelling result;
    result.text = path::parent(text);
    return result;
}

CanonicalPath::CanonicalPath(const Spelling& spelled) {
    // FIXME: the OS fails `missing/../a.h` where the fold of the part that
    // does not exist names an existing a.h: an absent input spelled so looks
    // present, and its PCH rebuilds on every edit.
    fs::weakly_canonical(text, spelled);
    path::canonicalize(text);
}

}  // namespace clice
