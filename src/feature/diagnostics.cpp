#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "support/filesystem.h"

namespace clice::feature {

namespace {

namespace protocol = eventide::language::protocol;

auto to_uri(llvm::StringRef file) -> std::string {
    if(path::is_absolute(file)) {
        return fs::toURI(file);
    }
    return file.str();
}

auto to_range(const PositionConverter& converter, LocalSourceRange range) -> protocol::Range {
    return protocol::Range{
        .start = converter.to_position(range.begin),
        .end = converter.to_position(range.end),
    };
}

auto to_severity(DiagnosticLevel level) -> std::optional<protocol::DiagnosticSeverity> {
    switch(level) {
        case DiagnosticLevel::Warning: return protocol::DiagnosticSeverity::Warning;
        case DiagnosticLevel::Error:
        case DiagnosticLevel::Fatal: return protocol::DiagnosticSeverity::Error;
        case DiagnosticLevel::Remark: return protocol::DiagnosticSeverity::Information;
        case DiagnosticLevel::Note: return protocol::DiagnosticSeverity::Hint;
        default: return std::nullopt;
    }
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
    PositionConverter converter(content, encoding);

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

    PositionConverter main_converter(unit.interested_content(), encoding);

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

        if(auto severity = to_severity(level)) {
            diagnostic.severity = *severity;
        }

        if(auto code = raw.id.diagnostic_code(); !code.empty()) {
            diagnostic.code = code.str();
        }

        if(auto uri = raw.id.diagnostic_document_uri()) {
            diagnostic.code_description = protocol::CodeDescription{.href = std::move(*uri)};
        }

        switch(raw.id.source) {
            case DiagnosticSource::Clang: diagnostic.source = "clang"; break;
            case DiagnosticSource::ClangTidy: diagnostic.source = "clang-tidy"; break;
            case DiagnosticSource::Clice: diagnostic.source = "clice"; break;
            case DiagnosticSource::Unknown: diagnostic.source = "unknown"; break;
        }

        add_tag(diagnostic, raw.id);

        if(raw.fid.isInvalid() || !raw.range.valid()) {
            current = std::move(diagnostic);
            continue;
        }

        if(raw.fid == unit.interested_file()) {
            diagnostic.range = to_range(main_converter, raw.range);
            current = std::move(diagnostic);
            continue;
        }

        auto include_location = unit.include_location(raw.fid);
        while(include_location.isValid()) {
            auto parent = unit.file_id(include_location);
            if(parent.isInvalid()) {
                break;
            }

            auto next = unit.include_location(parent);
            if(next.isInvalid()) {
                break;
            }

            include_location = next;
        }

        if(include_location.isInvalid()) {
            current = std::move(diagnostic);
            continue;
        }

        auto offset = unit.file_offset(include_location);
        auto end_offset = offset + unit.token_spelling(include_location).size();
        diagnostic.range = protocol::Range{
            .start = main_converter.to_position(offset),
            .end = main_converter.to_position(end_offset),
        };

        current = std::move(diagnostic);
    }

    flush();
    return result;
}

}  // namespace clice::feature
