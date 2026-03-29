#include <optional>
#include <string>
#include <vector>

#include "eventide/ipc/lsp/uri.h"
#include "feature/feature.h"

namespace clice::feature {

namespace {

namespace lsp = eventide::ipc::lsp;

auto to_uri(llvm::StringRef file) -> std::string {
    const auto file_view = std::string_view(file.data(), file.size());

    if(auto parsed = lsp::URI::parse(file_view)) {
        return parsed->str();
    }

    if(auto uri = lsp::URI::from_file_path(file_view)) {
        return uri->str();
    }

    return file.str();
}

void add_tag(protocol::Diagnostic& diagnostic, DiagnosticID id) {
    if(id.is_deprecated()) {
        if(!diagnostic.tags.has_value()) {
            diagnostic.tags = std::vector<protocol::DiagnosticTag>();
        }
        diagnostic.tags->push_back(protocol::DiagnosticTag::Deprecated);
    } else if(id.is_unused()) {
        if(!diagnostic.tags.has_value()) {
            diagnostic.tags = std::vector<protocol::DiagnosticTag>();
        }
        diagnostic.tags->push_back(protocol::DiagnosticTag::Unnecessary);
    }
}

void add_related(protocol::Diagnostic& diagnostic,
                 CompilationUnitRef unit,
                 const Diagnostic& raw,
                 PositionEncoding encoding) {
    if(raw.fid.isInvalid() || !raw.range.valid()) {
        return;
    }

    auto content = unit.file_content(raw.fid);
    PositionMapper converter(content, encoding);

    protocol::DiagnosticRelatedInformation related{
        .location =
            protocol::Location{
                               .uri = to_uri(unit.file_path(raw.fid)),
                               .range = to_range(converter, raw.range),
                               },
        .message = raw.message,
    };

    if(!diagnostic.related_information.has_value()) {
        diagnostic.related_information = std::vector<protocol::DiagnosticRelatedInformation>();
    }
    diagnostic.related_information->push_back(std::move(related));
}

}  // namespace

auto diagnostics(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::Diagnostic> {
    std::vector<protocol::Diagnostic> result;
    std::optional<protocol::Diagnostic> current;

    auto flush = [&]() {
        if(current.has_value()) {
            result.push_back(std::move(*current));
            current.reset();
        }
    };

    PositionMapper main_converter(unit.interested_content(), encoding);

    for(const auto& raw: unit.diagnostics()) {
        auto level = raw.id.level;

        if(level == DiagnosticLevel::Ignored) {
            continue;
        }

        if(level == DiagnosticLevel::Note || level == DiagnosticLevel::Remark) {
            if(current.has_value()) {
                add_related(*current, unit, raw, encoding);
            }
            continue;
        }

        flush();

        protocol::Diagnostic diagnostic{
            .range =
                protocol::Range{
                                .start = protocol::Position{.line = 0, .character = 0},
                                .end = protocol::Position{.line = 0, .character = 0},
                                },
            .message = raw.message,
        };

        if(level == DiagnosticLevel::Warning) {
            diagnostic.severity = protocol::DiagnosticSeverity::Warning;
        } else if(level == DiagnosticLevel::Error || level == DiagnosticLevel::Fatal) {
            diagnostic.severity = protocol::DiagnosticSeverity::Error;
        }

        if(auto code = raw.id.diagnostic_code(); !code.empty()) {
            diagnostic.code = code.str();
        }

        if(auto uri = raw.id.diagnostic_document_uri()) {
            diagnostic.code_description = protocol::CodeDescription{.href = std::move(*uri)};
        }

        // Keep legacy behavior: always report clang as source.
        diagnostic.source = "clang";

        add_tag(diagnostic, raw.id);

        if(raw.fid.isInvalid()) {
            diagnostic.range = protocol::Range{
                .start = protocol::Position{.line = 0, .character = 0},
                .end = protocol::Position{.line = 0, .character = 0},
            };
            current = std::move(diagnostic);
            continue;
        }

        if(raw.fid == unit.interested_file()) {
            diagnostic.range = to_range(main_converter, raw.range);
            current = std::move(diagnostic);
            continue;
        }

        auto include_location = unit.include_location(raw.fid);
        while(true) {
            auto parent = unit.file_id(include_location);
            if(parent.isValid()) {
                include_location = unit.include_location(parent);
            } else {
                break;
            }
        }

        auto offset = unit.file_offset(include_location);
        auto end_offset = offset + unit.token_spelling(include_location).size();
        diagnostic.range = protocol::Range{
            .start = *main_converter.to_position(offset),
            .end = *main_converter.to_position(end_offset),
        };

        current = std::move(diagnostic);
    }

    flush();
    return result;
}

}  // namespace clice::feature
