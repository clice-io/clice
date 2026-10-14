module;

#include "modules/prelude.h"

module clice:compile.tidy;

import :compile.dep_file;
import :compile.diagnostic;

namespace clice::tidy {

/// The frozen clang-tidy configuration of one run. Plain data on purpose:
/// it travels in worker requests and in the TURun family's product plan,
/// both of which freeze it at takeoff.
struct TidyParams {
    /// Effective checks glob list in clang-tidy syntax; empty falls back
    /// to the built-in default set.
    std::string checks;

    /// Check options (the .clang-tidy CheckOptions map).
    std::vector<std::pair<std::string, std::string>> options;

    /// Checks whose findings report as errors (WarningsAsErrors globs).
    std::string warnings_as_errors;

    /// Also report findings in headers matching this regex (clang-tidy's
    /// HeaderFilterRegex; empty = main file only).
    std::string header_filter;

    /// Headers excluded from reporting even when header_filter matches
    /// (clang-tidy's ExcludeHeaderFilterRegex).
    std::string exclude_header_filter;

    /// Also report findings in system headers.
    bool system_headers = false;

    /// What the checks take for headers and for sources by extension;
    /// unset keeps clang-tidy's defaults.
    std::optional<std::vector<std::string>> header_file_extensions;
    std::optional<std::vector<std::string>> implementation_file_extensions;

    /// Extra compiler args from the configuration. -W<group> flags are
    /// consumed engine-side by apply_warning_options so the Checks gate
    /// applies (the clangd approach, see tidy.cpp); a batch lint run
    /// additionally applies the remaining args (command_extra_args) to
    /// its driver command at resolution, as clang-tidy itself does. The
    /// interactive command is never rewritten.
    std::vector<std::string> extra_args;
    std::vector<std::string> extra_args_before;
};

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

    clang::DiagnosticsEngine::Level adjust_level(clang::DiagnosticsEngine::Level level,
                                                 const clang::Diagnostic& diag);

    void adjust_diag(Diagnostic& diag);
};

/// Configure clang-tidy over the compile `instance` runs. The batch shape
/// is clang-tidy's own: every configured check, the whole TU traversed,
/// suppression comments honored in every file, header findings kept for
/// the consumer's header filter, and a compiler warning a finding only
/// when Checks enables its clang-diagnostic-* name. Otherwise the editor
/// shape, clangd's: checks classified fast only, minus those unusable on
/// code mid-edit, over the main file's top-level declarations,
/// suppression comments read there only. `preamble_inactive` holds the
/// inactive regions of the consumed PCH's preamble share (flat begin/end
/// offset pairs), whose `#include`s the preamble replay skips.
std::unique_ptr<ClangTidyChecker> configure(clang::CompilerInstance& instance,
                                            const TidyParams& params,
                                            bool batch,
                                            llvm::ArrayRef<std::uint32_t> preamble_inactive);

}  // namespace clice::tidy
