module clice;

import :vfs.path;

namespace clice {

llvm::StringRef path::portable(llvm::StringRef p,
                               llvm::StringRef project_root,
                               llvm::SmallVectorImpl<char>& storage) {
    if(project_root.empty() || !is_within(p, project_root)) {
        return p;
    }
    auto rest = p.drop_front(project_root.size());
    storage.assign(workspace_anchor.begin(), workspace_anchor.end());
    if(!rest.empty() && !is_separator(rest.front())) {
        storage.push_back('/');
    }
    storage.append(rest.begin(), rest.end());
    return llvm::StringRef(storage.data(), storage.size());
}

llvm::StringRef path::local(llvm::StringRef name,
                            llvm::StringRef project_root,
                            llvm::SmallVectorImpl<char>& storage) {
    if(!name.consume_front(workspace_anchor)) {
        return name;
    }
    assert(!project_root.empty() && "a portable name read without its project root");
    storage.assign(project_root.begin(), project_root.end());
    if(project_root.ends_with("/")) {
        name.consume_front("/");
    }
    storage.append(name.begin(), name.end());
    return llvm::StringRef(storage.data(), storage.size());
}

Spelling::Spelling(llvm::StringRef text, const Spelling& base) {
    path::resolve(this->text, text, base.text, path::DotDot::Keep);
    path::canonicalize(this->text);
}

Spelling Spelling::absolute(llvm::StringRef text) {
    assert(path::is_absolute(text) && "an unanchored path");
    return Spelling(text, Spelling());
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

std::pair<std::size_t, std::size_t> path::tree_distance(llvm::StringRef path, llvm::StringRef other) {
    std::size_t shared = 0;
    auto n = std::min(path.size(), other.size());
    while(shared < n && path[shared] == other[shared]) {
        shared += 1;
    }
    auto separator = [](char c) {
        return path::is_separator(c);
    };
    while(shared > 0 && !separator(path[shared - 1])) {
        shared -= 1;
    }
    return {path.size() - shared,
            static_cast<std::size_t>(ranges::count_if(other.substr(shared), separator))};
}

}  // namespace clice
