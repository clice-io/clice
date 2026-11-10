/// A completion's look at the code after its point, which its own compile,
/// stopping there, does not read.

module clice:feature.code_completion.whole_buffer;

import :compile.compilation;
import :support.prelude;

namespace clice::feature {

/// The dependent expression at the completion point as the whole buffer's
/// compile parses it, in the innermost function holding it.
struct AtPoint {
    CompilationUnit& unit;

    clang::FunctionDecl* context = nullptr;

    clang::CXXDependentScopeMemberExpr* member = nullptr;

    /// Of a name a dependent scope qualifies, in an expression or a type.
    clang::NestedNameSpecifierLoc qualifier;
};

/// A second compile of the whole buffer a completion runs on, for what only
/// the code after the completion point tells. Its parse reads a name at the
/// point, where there may be none yet, so that the expression there parses
/// whole. Run at most once, when asked.
class WholeBuffer {
public:
    explicit WholeBuffer(const CompilationParams& completion);

    /// The expression at the point, the template `templated` around it
    /// instantiated as the code after the point first does: the compile
    /// skips the focus file's function bodies but those that may instantiate
    /// it, and stops at the call that does (CompilationParams::Instantiating).
    /// For the variable of a range-based `for`, `loop`, the statement there
    /// alone tells. None when nothing after the point calls the template, as
    /// `completion` has the macros before it, when the compile did not
    /// complete and when nothing dependent stands at the point.
    auto instantiated(const clang::NamedDecl* templated, bool loop, CompilationUnit& completion)
        -> std::optional<AtPoint>;

    /// The compile once it completed; null before.
    CompilationUnit* unit();

private:
    /// Whether the code after the point spells `name`, or a macro whose
    /// expansion does.
    bool called_after(llvm::StringRef name, clang::Preprocessor& pp);

private:
    CompilationInput input;

    std::uint32_t point;

    /// Where the name its parse reads at the point starts.
    std::uint32_t target = 0;

    std::shared_ptr<std::atomic_bool> stop;

    std::unique_ptr<CompilationUnit> compiled;
};

}  // namespace clice::feature
