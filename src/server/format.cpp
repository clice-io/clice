#include "server/format.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "compile/diagnostic.h"
#include "feature/feature.h"

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
    DiagnosticID id{
        .value = 0,
        .level = DiagnosticLevel::Warning,
        .source = DiagnosticSource::Clice,
        .name = "inferred-compile-command",
    };

    auto diagnostic = feature::file_warning(std::format(
        "No compilation database entry for this file (compile command was {}), so some includes "
        "may not be found. Configure compile_commands.json for accurate diagnostics.",
        source == CommandSource::Fallback   ? "synthesized from defaults"
        : source == CommandSource::Inferred ? "borrowed from a nearby translation unit"
                                            : "inferred from an including file"));
    diagnostic.code = id.name.str();
    if(auto uri = id.diagnostic_document_uri()) {
        diagnostic.code_description = protocol::CodeDescription{.href = std::move(*uri)};
    }
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
    if(output.source != CommandSource::CDBExact && output.source != CommandSource::Default &&
       std::ranges::any_of(diagnostics, is_file_not_found)) {
        diagnostics.insert(diagnostics.begin(), make_inferred_command_diagnostic(output.source));
    }

    return diagnostics;
}

}  // namespace clice
