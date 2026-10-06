module;

#include "modules/prelude.h"

module clice:vfs.path;

import :support.format;

namespace clice {
namespace path {

using namespace llvm::sys::path;

/// Whether `p` is spelled from a root, so no anchor may be prepended:
/// absolute in the native style, or rooted without a drive (`/x`), which
/// Windows resolves against the current drive and `is_absolute` rejects.
inline bool is_rooted(llvm::StringRef p) {
    return is_absolute(p) || has_root_directory(p);
}

/// Whether `p` is `root` or lies under it.
inline bool under(llvm::StringRef p, llvm::StringRef root) {
    return p == root ||
           (p.starts_with(root) && (root.ends_with("/") || is_separator(p[root.size()])));
}

/// Visit `start` and its ancestors, nearest first, until `visit` returns
/// false or `stop` has been visited (the filesystem root when empty).
inline void walk_ancestors(llvm::StringRef start,
                           llvm::StringRef stop,
                           llvm::function_ref<bool(llvm::StringRef)> visit) {
    while(stop.size() > 1 && is_separator(stop.back())) {
        stop = stop.drop_back();
    }
    for(llvm::StringRef dir = start; !dir.empty();) {
        if(!visit(dir) || dir == stop) {
            return;
        }
        auto parent = parent_path(dir);
        if(parent.size() == dir.size()) {
            return;
        }
        dir = parent;
    }
}

template <typename... Args>
std::string join(Args&&... args) {
    llvm::SmallString<128> path;
    ((path::append(path, std::forward<Args>(args))), ...);
    return path.str().str();
}

/// Windows accepts either separator and either drive case; LSP clients
/// (vscode-uri) key documents by the lowercase-drive, forward-slash
/// spelling, so every path clice keeps or emits is spelled that way. On
/// POSIX a path is its raw bytes — '\' and "C:" are ordinary filename
/// characters — and canonicalization must not touch it.

/// True when `p` deviates from the Windows canonical spelling.
inline bool needs_canonical(llvm::StringRef p) {
    return p.contains('\\') || (p.size() >= 2 && p[1] == ':' && llvm::isUpper(p[0]));
}

/// The rewrite itself, in place. Platform-independent on purpose so the
/// logic is unit-testable on any host; production code reaches it only
/// through the gated entry points below.
inline void make_canonical(llvm::MutableArrayRef<char> p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    if(p.size() >= 2 && p[1] == ':' && llvm::isUpper(p[0])) {
        p[0] = llvm::toLower(p[0]);
    }
}

/// Canonical view of `p`: on Windows, `p` itself when already canonical,
/// otherwise the rewrite materialized in `storage`; on POSIX always `p`.
inline llvm::StringRef canonical(llvm::StringRef p,
                                 [[maybe_unused]] llvm::SmallVectorImpl<char>& storage) {
#ifdef _WIN32
    if(needs_canonical(p)) {
        storage.assign(p.begin(), p.end());
        make_canonical(storage);
        return llvm::StringRef(storage.data(), storage.size());
    }
#endif
    return p;
}

/// In-place canonicalization of an owned string (config dirs, the
/// workspace root). No-op on POSIX.
inline void canonicalize([[maybe_unused]] std::string& p) {
#ifdef _WIN32
    if(needs_canonical(p)) {
        make_canonical(llvm::MutableArrayRef(p.data(), p.size()));
    }
#endif
}

}  // namespace path

struct FileTable;
class CanonicalRef;
class CanonicalPath;

/// A path the way an input wrote it, joined onto the directory the input's
/// relative paths are relative to: absolute, canonically spelled, without
/// `.` segments or a trailing separator. `..` stays: the OS resolves it
/// past symlinks, so only the identity (CanonicalPath) interprets it. Kept
/// where a lookup depends on how a path was written — the directory a
/// quoted include starts from, the file a rendered command names. Two
/// spellings compare as text; only identities (CanonicalPath) tell whether
/// they name one file.
class Spelling {
public:
    /// None: no path.
    Spelling() = default;

    /// `text` as an input wrote it, relative to `base`; absolute text
    /// stands alone.
    Spelling(llvm::StringRef text, const Spelling& base);

    /// A path already absolute: clang's file names, the worker protocol,
    /// persisted paths.
    static Spelling absolute(llvm::StringRef text);

    /// The directory command-line arguments are relative to.
    static Spelling cwd();

    /// An identity spells itself.
    explicit Spelling(CanonicalRef identity);

    operator llvm::StringRef() const {
        return text;
    }

    /// For LLVM's file APIs; points into this object, so it lives for the
    /// call it is passed to.
    operator llvm::Twine() const {
        return llvm::Twine(text);
    }

    const std::string& str() const {
        return text;
    }

    bool empty() const {
        return text.empty();
    }

    /// The directory holding it, as spelled.
    Spelling parent() const;

    friend bool operator==(const Spelling&, const Spelling&) = default;

private:
    std::string text;
};

/// A path naming a file or directory by its identity, the way the file
/// table and the worker do: the name the OS gives its longest existing
/// prefix, the rest appended as spelled with `.`/`..` removed, canonically
/// spelled. The OS's name follows symlinks everywhere; on Windows it also
/// carries the on-disk case and follows junctions and subst drives, which
/// is exactly what clang merges into one file there. Two spellings of one
/// file compare equal whether it exists yet or not.
///
/// Only resolution (CanonicalPath's constructor), the file table and the
/// derivations below (parent(), and entry() on its caller's word) make
/// one, and one never compares with a plain string: a spelling taken for
/// an identity is a compile error. Either reads as a plain string wherever
/// a spelling will do.
class CanonicalRef {
public:
    CanonicalRef() = default;

    operator llvm::StringRef() const {
        return text;
    }

    /// For LLVM's file APIs; points into this object, so it lives for the
    /// call it is passed to.
    operator llvm::Twine() const {
        return llvm::Twine(text);
    }

    explicit operator std::string() const {
        return text.str();
    }

    std::string str() const {
        return text.str();
    }

    /// Null-terminated.
    const char* data() const {
        return text.data();
    }

    std::size_t size() const {
        return text.size();
    }

    bool empty() const {
        return text.empty();
    }

    /// The directory holding it, itself an identity.
    CanonicalPath parent() const;

    /// A path a directory walk from here reached without following a
    /// symlink, itself an identity: every component below this one is a
    /// real directory entry.
    CanonicalPath entry(llvm::StringRef path) const;

private:
    friend class CanonicalPath;
    friend struct FileTable;

    explicit CanonicalRef(llvm::StringRef text) : text(text) {}

    llvm::StringRef text;
};

class CanonicalPath {
public:
    CanonicalPath() = default;

    /// The identity of what `spelled` names.
    explicit CanonicalPath(const Spelling& spelled);

    CanonicalPath(CanonicalRef ref) : text(ref.str()) {}

    operator CanonicalRef() const {
        return CanonicalRef(text);
    }

    operator llvm::StringRef() const {
        return text;
    }

    /// For LLVM's file APIs; points into this object, so it lives for the
    /// call it is passed to.
    operator llvm::Twine() const {
        return llvm::Twine(text);
    }

    const std::string& str() const {
        return text;
    }

    std::size_t size() const {
        return text.size();
    }

    bool empty() const {
        return text.empty();
    }

private:
    friend class CanonicalRef;

    struct Resolved {};

    CanonicalPath(Resolved, llvm::StringRef text) : text(text) {}

    std::string text;
};

/// An identity: a CanonicalRef, a CanonicalPath, or a type wrapping one
/// by inheritance (a reflected configuration field).
template <typename T>
concept Canonical = std::same_as<T, CanonicalRef> || std::derived_from<T, CanonicalPath>;

template <Canonical L, Canonical R>
bool operator==(const L& lhs, const R& rhs) {
    return llvm::StringRef(lhs) == llvm::StringRef(rhs);
}

template <Canonical L, Canonical R>
std::strong_ordering operator<=>(const L& lhs, const R& rhs) {
    return llvm::StringRef(lhs).compare(llvm::StringRef(rhs)) <=> 0;
}

/// An identity compares with an identity only.
template <typename L, typename R>
    requires (Canonical<L> != Canonical<R>)
bool operator==(const L& lhs, const R& rhs) = delete;

template <typename L, typename R>
    requires (Canonical<L> != Canonical<R>)
std::strong_ordering operator<=>(const L& lhs, const R& rhs) = delete;

namespace path {

/// How configuration and persisted names spell the workspace root.
constexpr inline llvm::StringRef workspace_anchor = "${workspace}";

/// The name records that outlive the checkout's location give a path:
/// `${workspace}/rel` under the workspace root, so the index database and
/// the hashes of symbols and commands survive a move of the checkout; any
/// other path, and every path without a workspace, stays as it is. Points
/// into `p` or `storage`.
llvm::StringRef portable(llvm::StringRef p,
                         llvm::StringRef workspace,
                         llvm::SmallVectorImpl<char>& storage);

/// The path a portable name names in the checkout at `workspace`. Points
/// into `name` or `storage`.
llvm::StringRef local(llvm::StringRef name,
                      llvm::StringRef workspace,
                      llvm::SmallVectorImpl<char>& storage);

/// Whether the identity `p` is `root` or lies under it.
template <Canonical P, Canonical R>
bool under(const P& p, const R& root) {
    return under(llvm::StringRef(p), llvm::StringRef(root));
}

/// An identity lies under an identity only.
template <typename P, typename R>
    requires (Canonical<P> != Canonical<R>)
bool under(const P& p, const R& root) = delete;

}  // namespace path

inline CanonicalPath CanonicalRef::parent() const {
    return CanonicalPath(CanonicalPath::Resolved{}, path::parent_path(text));
}

inline CanonicalPath CanonicalRef::entry(llvm::StringRef path) const {
    assert(path::under(path, text));
    return CanonicalPath(CanonicalPath::Resolved{}, path);
}

namespace path {

/// Visit the identity `start` and its ancestors, nearest first, until
/// `visit` returns false.
inline void walk_ancestors(CanonicalRef start, llvm::function_ref<bool(CanonicalRef)> visit) {
    for(CanonicalPath dir = start; visit(dir);) {
        auto parent = CanonicalRef(dir).parent();
        if(parent.empty() || parent.size() == dir.size()) {
            return;
        }
        dir = std::move(parent);
    }
}

}  // namespace path

}  // namespace clice

/// Not every Canonical type: an annotated field formats through kotatsu's
/// annotation formatter, which defers to this one.
template <typename T>
    requires std::same_as<T, clice::CanonicalRef> || std::same_as<T, clice::CanonicalPath>
struct std::formatter<T> : std::formatter<llvm::StringRef> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const {
        return std::formatter<llvm::StringRef>::format(llvm::StringRef(value), ctx);
    }
};

template <>
struct std::formatter<clice::Spelling> : std::formatter<llvm::StringRef> {
    template <typename FormatContext>
    auto format(const clice::Spelling& value, FormatContext& ctx) const {
        return std::formatter<llvm::StringRef>::format(llvm::StringRef(value), ctx);
    }
};
