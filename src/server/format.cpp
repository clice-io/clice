module;

#include "support/prelude.h"

module clice;

import :compile.diagnostic;
import :feature.diagnostics;
import :server.format;

namespace clice {

/// Clang diagnostics indicating that an input file could not be found —
/// the symptom of a guessed compile command missing include paths.
static bool is_file_not_found(const feature::FileDiagnostic& diagnostic) {
    return diagnostic.origin != feature::DiagnosticOrigin::IncluderContext &&
           (diagnostic.id == clang::diag::err_pp_file_not_found ||
            diagnostic.id == clang::diag::err_pp_error_opening_file ||
            diagnostic.id == clang::diag::err_module_not_found);
}

/// File-top warning explaining that diagnostics were produced with a guessed
/// compile command, pointing at the compilation database documentation.
static feature::FileDiagnostic make_inferred_command_diagnostic(CommandSource source) {
    auto diagnostic = feature::file_warning(std::format(
        "No compilation database entry for this file (compile command was {}), so some includes "
        "may not be found. Configure compile_commands.json for accurate diagnostics.",
        source == CommandSource::Fallback   ? "synthesized from defaults"
        : source == CommandSource::Inferred ? "borrowed from a nearby translation unit"
                                            : "inferred from an including file"));
    diagnostic.code = "inferred-compile-command";
    diagnostic.link = "https://docs.clice.io/clice/guide/quick-start#project-setup";
    return diagnostic;
}

/// File-top warning naming the host whose includer context the header
/// needed but could not get: the diagnostics below miss what it provides.
static feature::FileDiagnostic make_unmatched_context_diagnostic(llvm::StringRef host) {
    auto diagnostic = feature::file_warning(std::format(
        "This header needs the context of its includer, but the include chain from {} could not "
        "be followed under that file's compile command, so it compiled without it.",
        host));
    diagnostic.code = "unmatched-includer-context";
    return diagnostic;
}

std::vector<feature::FileDiagnostic>
    format_diagnostics(const CompileOutput& output, const feature::DiagnosticsOptions& options) {
    // Of the diagnostics moved from other files to one spot, the first
    // error and the first warning show: a broken header's every error
    // would pile up on its #include.
    std::set<std::tuple<std::uint32_t, std::uint32_t, bool>> relocated;
    auto diagnostics = output.diagnostics;
    diagnostics.append_range(output.skipped);
    std::erase_if(diagnostics, [&](const feature::FileDiagnostic& diagnostic) {
        auto start = diagnostic.range.start;
        return ranges::contains(options.suppress, diagnostic.code) ||
               (diagnostic.origin != feature::DiagnosticOrigin::File &&
                !relocated
                     .insert(
                         {start.line, start.character, diagnostic.level >= DiagnosticLevel::Error})
                     .second);
    });

    // Guidance (and only when it can explain something): an exact CDB match
    // never gets the note, and neither does a guessed command that worked.
    // The includer's own code compiles under its own command: what it
    // misses, no command for the file would find.
    if(output.source != CommandSource::CDBExact && output.source != CommandSource::Default &&
       ranges::any_of(diagnostics, is_file_not_found)) {
        diagnostics.insert(diagnostics.begin(), make_inferred_command_diagnostic(output.source));
    }
    if(!output.unmatched_host.empty()) {
        diagnostics.insert(diagnostics.begin(),
                           make_unmatched_context_diagnostic(output.unmatched_host));
    }

    return diagnostics;
}

}  // namespace clice
