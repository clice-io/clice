module;

#include "modules/prelude.h"

module clice:compile.tidy;

import :compile.compilation;
import :compile.diagnostic;

namespace clice::tidy {

/// The effective clang-tidy configuration for a file, and the places the
/// resolution looked for a .clang-tidy: those it read, by the hash of
/// their text, and those it found none at — a change at any of them can
/// change the configuration.
struct TidyResolution {
    TidyParams params;
    std::vector<DepFile> files;

    /// Why a configuration file on the way did not parse, as clang-tidy
    /// reports it; the resolution reads on past it as clang-tidy does.
    std::string error;
};

/// Resolve the effective clang-tidy configuration for `file` from its
/// nearest .clang-tidy files (clang-tidy's own search and inheritance
/// semantics). Files without any configuration resolve to empty checks —
/// the consumer's default set applies.
TidyResolution resolve_tidy_params(llvm::StringRef file);

/// The plan's compilation-affecting extra args, split for clang-tidy's
/// own insertion points on the driver command: extra_args_before prepend
/// right after the binary name, extra_args append at the end. Applied
/// BEFORE toolchain resolution, so the driver itself interprets
/// pass-throughs and driver-only options (-Wp,, -Xpreprocessor,
/// --target) when it produces the cc1 line.
struct CommandExtraArgs {
    std::vector<std::string> prepend;
    std::vector<std::string> append;
};

/// Split the plan's extra args into the command-affecting halves. -W
/// warning flags are withheld — they reach the diagnostics engine through
/// apply_warning_options, where the Checks gate applies; on the command
/// they would bypass it. -Wp,/-Wl,/-Wa, are driver pass-throughs, not
/// warning flags, and stay in.
CommandExtraArgs command_extra_args(llvm::ArrayRef<std::string> extra_args,
                                    llvm::ArrayRef<std::string> extra_args_before);

/// The clang-tidy pass over one compile.
class ClangTidyChecker {
public:
    /// The context of the clang-tidy checker.
    clang::tidy::ClangTidyContext context;

    /// A batch run's findings, the checks' and the compiler's, kept or left
    /// out as clang-tidy keeps them (see configure).
    clang::tidy::ClangTidyDiagnosticConsumer findings{context};

    /// The engine the checks report through in a batch run (see
    /// configure); the compiler's otherwise.
    std::unique_ptr<clang::DiagnosticsEngine> engine;

    /// The instances of checks that are enabled for the current Language.
    std::vector<std::unique_ptr<clang::tidy::ClangTidyCheck>> checks;

    /// The match finder to run clang-tidy on ASTs.
    clang::ast_matchers::MatchFinder finder;

    /// See configure.
    bool batch = false;

    /// The check names the unit's diagnostics borrow.
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver names{allocator};

    ClangTidyChecker(std::unique_ptr<clang::tidy::ClangTidyOptionsProvider> provider,
                     clang::ast_matchers::MatchFinder::MatchFinderOptions options);

    /// The editor shape's level for a diagnostic: Ignored when a suppression
    /// comment in the focus file or a system macro hides it, Error for a
    /// warning WarningsAsErrors names.
    clang::DiagnosticsEngine::Level adjust_level(clang::DiagnosticsEngine::Level level,
                                                 const clang::Diagnostic& diag);

    /// Name a check's diagnostic after its check, in the editor shape.
    void adjust_diag(Diagnostic& diag);
};

/// Configure clang-tidy over the compile `instance` runs. The batch shape
/// is clang-tidy's own: every configured check over the whole TU, and
/// clang-tidy's consumer collecting the findings, which applies the
/// suppression comments, the header filters and the Checks a compiler
/// warning needs to be a finding. Otherwise the editor shape, clangd's:
/// checks classified fast only, minus those unusable on code mid-edit,
/// over the focus file's top-level declarations, suppression comments read
/// there only. `preamble_inactive` holds the inactive regions of the
/// consumed PCH's preamble share (flat begin/end offset pairs), whose
/// `#include`s the preamble replay skips.
std::unique_ptr<ClangTidyChecker> configure(clang::CompilerInstance& instance,
                                            const TidyParams& params,
                                            bool batch,
                                            llvm::ArrayRef<std::uint32_t> preamble_inactive);

}  // namespace clice::tidy
