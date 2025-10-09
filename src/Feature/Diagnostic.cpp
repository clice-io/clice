#include "Feature/Diagnostic.h"
#include "Compiler/CompilationUnit.h"
#include "Server/Convert.h"
#include "Support/Logging.h"

namespace clice::feature {

json::Value diagnostics(PositionEncodingKind kind, PathMapping mapping, CompilationUnit& unit) {
    json::Array result;

    std::optional<proto::Diagnostic> diagnostic;

    auto flush = [&]() {
        if(diagnostic) {
            /// FIXME: We should use a better way?
            result.emplace_back(json::serialize(*diagnostic));
            diagnostic.reset();
        }
    };

    for(auto& raw_diagnostic: unit.diagnostics()) {
        auto level = raw_diagnostic.id.level;
        auto fid = raw_diagnostic.fid;

        /// FIXME: Is it possible that a group of notes following the
        /// ignored diagnostic? so that we should skil them also.
        if(level == DiagnosticLevel::Ignored) {
            continue;
        }

        /// Append to last.
        if(level == DiagnosticLevel::Note || level == DiagnosticLevel::Remark) {
            /// FIXME: figure out why it may be invalid.
            if(fid.isInvalid()) {
                logging::info("code: {}, message: {}",
                              raw_diagnostic.id.diagnostic_code(),
                              raw_diagnostic.message);
                continue;
            }

            if(!raw_diagnostic.range.valid()) {
                continue;
            }

            auto content = unit.file_content(fid);
            PositionConverter converter(content, kind);

            proto::Location location;
            location.range.start = converter.toPosition(raw_diagnostic.range.begin);
            location.range.end = converter.toPosition(raw_diagnostic.range.end);
            location.uri = mapping.to_uri(unit.file_path(fid));

            diagnostic->relatedInformation.emplace_back(std::move(location),
                                                        raw_diagnostic.message);
            continue;
        }

        /// Flash the last diagnostic.
        flush();
        diagnostic.emplace();

        /// If the fid is invalid, we add a default range for it.
        if(fid.isInvalid()) {
            diagnostic->range = {0, 0, 0, 0};
        } else if(fid == unit.interested_file()) {
            PositionConverter converter(unit.interested_content(), kind);
            diagnostic->range.start = converter.toPosition(raw_diagnostic.range.begin);
            diagnostic->range.end = converter.toPosition(raw_diagnostic.range.end);
        } else {
            PositionConverter converter(unit.interested_content(), kind);

            /// Get the top level include location.
            auto include_location = unit.include_location(fid);
            while(true) {
                auto fid2 = unit.file_id(include_location);
                if(fid2.isValid()) {
                    include_location = unit.include_location(fid2);
                } else {
                    break;
                }
            }

            /// Use the location of include directive.
            auto offset = unit.file_offset(include_location);
            auto end_offset = offset + unit.token_spelling(include_location).size();

            diagnostic->range.start = converter.toPosition(offset);
            diagnostic->range.end = converter.toPosition(end_offset);
        }

        if(level == DiagnosticLevel::Warning) {
            diagnostic->severity = proto::DiagnosticSeverity::Warning;
        } else if(level == DiagnosticLevel::Error || level == DiagnosticLevel::Fatal) {
            diagnostic->severity = proto::DiagnosticSeverity::Error;
        }

        diagnostic->code = raw_diagnostic.id.diagnostic_code();

        if(auto uri = raw_diagnostic.id.diagnostic_document_uri()) {
            /// It is already be uri, mapping is not needed.
            diagnostic->codeDescription.emplace(std::move(*uri));
        }

        /// FIXME: According to raw_diagnostic.id.source to assign,
        /// currently all diagnostics are from clang.
        diagnostic->source = "clang";

        diagnostic->message = raw_diagnostic.message;

        if(raw_diagnostic.id.is_deprecated()) {
            diagnostic->tags.emplace_back(proto::DiagnosticTag::Deprecated);
        } else if(raw_diagnostic.id.is_unused()) {
            diagnostic->tags.emplace_back(proto::DiagnosticTag::Unnecessary);
        }
    }

    flush();

    return result;
}

}  // namespace clice::feature
