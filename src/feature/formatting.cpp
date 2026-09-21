#include <expected>
#include <format>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "support/logging.h"

#include "llvm/Support/Error.h"
#include "clang/Format/Format.h"

namespace clice::feature {

namespace {
namespace tooling = clang::tooling;

auto file_style(llvm::StringRef file) -> std::expected<clang::format::FormatStyle, std::string> {
    // Set code to empty to avoid meaningless file type guess.
    auto style = clang::format::getStyle(clang::format::DefaultFormatStyle,
                                         file,
                                         clang::format::DefaultFallbackStyle,
                                         "");
    if(!style) {
        return std::unexpected(llvm::toString(style.takeError()));
    }
    return std::move(*style);
}

auto format_content(llvm::StringRef file, llvm::StringRef content, tooling::Range range)
    -> std::expected<tooling::Replacements, std::string> {
    auto style = file_style(file);
    if(!style) {
        return std::unexpected(std::move(style.error()));
    }

    std::vector<tooling::Range> ranges = {range};
    auto include_replacements = clang::format::sortIncludes(*style, content, ranges, file);
    auto changed = tooling::applyAllReplacements(content, include_replacements);
    if(!changed) {
        return std::unexpected(llvm::toString(changed.takeError()));
    }

    return include_replacements.merge(clang::format::reformat(
        *style,
        *changed,
        tooling::calculateRangesAfterReplacements(include_replacements, ranges)));
}

auto to_edits(const tooling::Replacements& replacements) -> std::vector<TextReplacement> {
    std::vector<TextReplacement> edits;
    for(const auto& replacement: replacements) {
        auto begin = static_cast<std::uint32_t>(replacement.getOffset());
        edits.push_back({
            .range = {begin, static_cast<std::uint32_t>(begin + replacement.getLength())},
            .text = replacement.getReplacementText().str()
        });
    }
    return edits;
}

}  // namespace

auto document_format(llvm::StringRef file,
                     llvm::StringRef content,
                     std::optional<LocalSourceRange> range,
                     PositionEncoding encoding) -> std::vector<protocol::TextEdit> {
    std::vector<protocol::TextEdit> edits;

    auto selection =
        range ? tooling::Range(range->begin, range->length()) : tooling::Range(0, content.size());
    auto replacements = format_content(file, content, selection);
    if(!replacements) {
        LOG_WARN("Failed to format {}: {}", file, replacements.error());
        return edits;
    }

    LineMap map(content, encoding);

    for(const auto& replacement: *replacements) {
        auto begin = static_cast<std::uint32_t>(replacement.getOffset());
        auto end = static_cast<std::uint32_t>(begin + replacement.getLength());
        auto range = to_range(map, {begin, end});
        if(!range)
            continue;
        protocol::TextEdit edit{
            .range = *range,
            .new_text = replacement.getReplacementText().str(),
        };
        edits.push_back(std::move(edit));
    }

    return edits;
}

auto format_edits(llvm::StringRef file, llvm::StringRef content, std::vector<TextReplacement> edits)
    -> std::vector<TextReplacement> {
    auto style = file_style(file);
    if(!style) {
        LOG_WARN("Failed to load the format style of {}: {}", file, style.error());
        return edits;
    }
    if(style->DisableFormat) {
        return edits;
    }

    tooling::Replacements replacements;
    std::vector<tooling::Range> ranges;
    for(const auto& edit: edits) {
        auto error = replacements.add(
            tooling::Replacement(file, edit.range.begin, edit.range.length(), edit.text));
        if(error) {
            LOG_WARN("Overlapping edits in {}: {}", file, llvm::toString(std::move(error)));
            return edits;
        }
        ranges.emplace_back(edit.range.begin, edit.range.length());
    }
    auto changed = tooling::applyAllReplacements(content, replacements);
    if(!changed) {
        LOG_WARN("Failed to apply edits of {}: {}", file, llvm::toString(changed.takeError()));
        return edits;
    }
    auto formatted =
        clang::format::reformat(*style,
                                *changed,
                                tooling::calculateRangesAfterReplacements(replacements, ranges),
                                file);
    return to_edits(replacements.merge(formatted));
}

auto format_snippet(llvm::StringRef file, llvm::StringRef text) -> std::string {
    auto style = file_style(file);
    if(!style || style->DisableFormat) {
        return text.str();
    }
    std::vector<tooling::Range> ranges = {
        tooling::Range(0, static_cast<unsigned>(text.size())),
    };
    auto formatted =
        tooling::applyAllReplacements(text, clang::format::reformat(*style, text, ranges, file));
    return formatted ? std::move(*formatted) : text.str();
}

}  // namespace clice::feature
