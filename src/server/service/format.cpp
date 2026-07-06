#include "server/service/format.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "compile/diagnostic.h"
#include "server/compiler/context_resolver.h"
#include "support/logging.h"

#include "kota/codec/json/json.h"

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

    protocol::Diagnostic diagnostic;
    diagnostic.range = protocol::Range{
        .start = protocol::Position{.line = 0, .character = 0},
        .end = protocol::Position{.line = 0, .character = 0},
    };
    diagnostic.severity = protocol::DiagnosticSeverity::Warning;
    diagnostic.code = id.name.str();
    if(auto uri = id.diagnostic_document_uri()) {
        diagnostic.code_description = protocol::CodeDescription{.href = std::move(*uri)};
    }
    diagnostic.source = "clice";
    diagnostic.message = std::format(
        "No compilation database entry for this file (compile command was {}), so some includes " "may not be found. Configure compile_commands.json for accurate diagnostics.",
        source == CommandSource::Fallback ? "synthesized from defaults"
                                          : "inferred from an including file");
    return diagnostic;
}

std::vector<protocol::Diagnostic> format_diagnostics(const CompileOutput& output) {
    std::vector<protocol::Diagnostic> diagnostics;
    if(!output.diagnostics.empty()) {
        auto status = kota::codec::json::from_json(output.diagnostics.data, diagnostics);
        if(!status) {
            LOG_WARN("Failed to deserialize diagnostics JSON");
        }
    }

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
    if(output.source != CommandSource::CDBExact &&
       std::ranges::any_of(diagnostics, is_file_not_found)) {
        diagnostics.insert(diagnostics.begin(), make_inferred_command_diagnostic(output.source));
    }

    return diagnostics;
}

std::vector<protocol::Range> format_inactive_regions(const Session& session,
                                                     const CompileOutput& output) {
    std::vector<protocol::Range> result;
    if(!output.inactive_regions.has_value()) {
        return result;
    }
    auto& regions = *output.inactive_regions;
    auto map = session.line_map();
    result.reserve(regions.size() / 2);
    for(std::size_t i = 0; i + 1 < regions.size(); i += 2) {
        auto start = map.to_position(regions[i]);
        auto end = map.to_position(regions[i + 1]);
        if(!start || !end) {
            continue;
        }
        protocol::Range range;
        range.start = *start;
        range.end = *end;
        result.push_back(range);
    }
    return result;
}

}  // namespace clice
