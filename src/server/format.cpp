module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :server.format;

namespace clice {

/// Clang diagnostics indicating that an input file could not be found —
/// the symptom of a guessed compile command missing include paths.
static bool is_file_not_found(const protocol::Diagnostic& diagnostic) {
    if(!diagnostic.code.has_value())
        return false;
    auto* code = std::get_if<protocol::string>(&*diagnostic.code);
    return code && (*code == "err_pp_file_not_found" || *code == "err_pp_error_opening_file" ||
                    *code == "err_module_not_found");
}

/// File-top warning explaining that diagnostics were produced with a guessed
/// compile command, pointing at the compilation database documentation.
static protocol::Diagnostic make_inferred_command_diagnostic(CommandSource source) {
    auto diagnostic = feature::file_warning(std::format(
        "No compilation database entry for this file (compile command was {}), so some includes "
        "may not be found. Configure compile_commands.json for accurate diagnostics.",
        source == CommandSource::Fallback   ? "synthesized from defaults"
        : source == CommandSource::Inferred ? "borrowed from a nearby translation unit"
                                            : "inferred from an including file"));
    diagnostic.code = "inferred-compile-command";
    diagnostic.code_description = protocol::CodeDescription{
        .href = "https://docs.clice.io/clice/guide/quick-start#project-setup",
    };
    return diagnostic;
}

/// File-top warning naming the host whose includer context the header
/// needed but could not get: the diagnostics below miss what it provides.
static protocol::Diagnostic make_unmatched_context_diagnostic(llvm::StringRef host) {
    auto diagnostic = feature::file_warning(std::format(
        "This header needs the context of its includer, but the include chain from {} could not "
        "be followed under that file's compile command, so it compiled without it.",
        host));
    diagnostic.code = "unmatched-includer-context";
    return diagnostic;
}

std::vector<protocol::Diagnostic> format_diagnostics(const CompileOutput& output) {
    auto diagnostics = output.diagnostics;

    // Suffix injection appends an #include past the user's EOF; errors in
    // host code after the include point remap onto those phantom lines.
    // They describe the includer, not the document — drop them.
    if(output.line_limit.has_value()) {
        std::erase_if(diagnostics, [&](const protocol::Diagnostic& d) {
            return d.range.start.line >= *output.line_limit;
        });
    }

    // Guidance (and only when it can explain something): an exact CDB match
    // never gets the note, and neither does a guessed command that worked.
    // The includer's own code compiles under its own command: what it
    // misses, no command for the file would find.
    if(output.source != CommandSource::CDBExact && output.source != CommandSource::Default &&
       std::ranges::any_of(diagnostics, [](const protocol::Diagnostic& diagnostic) {
           return is_file_not_found(diagnostic) && !feature::in_includer_context(diagnostic);
       })) {
        diagnostics.insert(diagnostics.begin(), make_inferred_command_diagnostic(output.source));
    }
    if(!output.unmatched_host.empty()) {
        diagnostics.insert(diagnostics.begin(),
                           make_unmatched_context_diagnostic(output.unmatched_host));
    }

    return diagnostics;
}

}  // namespace clice
