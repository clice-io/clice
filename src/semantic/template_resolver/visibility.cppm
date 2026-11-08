module clice:semantic.template_resolver.visibility;

import :compile.compilation;
import :support.prelude;

namespace clice::types {

/// What a place in a file of the unit sees on its own: the declarations of
/// its file and of what the file includes or imports before the place, as
/// if the file were compiled alone. What the units including a header
/// declare before it differs from one unit to the next; the focus file is
/// compiled alone and sees the whole unit.
class Visibility {
public:
    explicit Visibility(CompilationUnit& unit) : unit(unit) {}

    /// See from `location` on; from a place in no file, the whole unit.
    void look_from(clang::SourceLocation location);

    /// Whether the place looked from sees `decl`.
    bool sees(const clang::NamedDecl* decl);

private:
    /// What a file brings in itself: it, every file it includes with what
    /// those include in turn, and the modules they import.
    struct Reach {
        llvm::DenseSet<clang::FileID> files;

        llvm::StringSet<> modules;
    };

    /// Add the modules an import of `name` makes visible: it, and those it
    /// re-exports, in turn.
    void add_imported(llvm::StringRef name, llvm::StringSet<>& modules);

    const Reach& reach(clang::FileID file);

    /// The offset in `file` from which `decl` is visible on its own: 0 when
    /// the file declares it or it is a function of the standard library
    /// whose meaning is fixed (decls::fixed_std_utility), past the first
    /// import or include bringing a declaration in, else none.
    std::uint32_t first_visible(const clang::NamedDecl* decl, clang::FileID file);

private:
    CompilationUnit& unit;

    /// The place looked from; none for the whole unit.
    clang::FileID file;

    std::uint32_t offset = 0;

    /// What each file brings in itself (reach).
    llvm::DenseMap<clang::FileID, Reach> reached;

    /// Where in a file a declaration becomes visible on its own
    /// (first_visible).
    llvm::DenseMap<std::pair<clang::FileID, const clang::Decl*>, std::uint32_t> visible_from;
};

}  // namespace clice::types
