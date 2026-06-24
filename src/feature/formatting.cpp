#include <expected>
#include <format>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "support/logging.h"

#include "clang/Format/Format.h"

namespace clice::feature {

namespace {
namespace tooling = clang::tooling;

auto format_content(llvm::StringRef file, llvm::StringRef content, tooling::Range range)
    -> std::expected<tooling::Replacements, std::string> {
    // Set code to empty to avoid meaningless file type guess.
    auto style = clang::format::getStyle(clang::format::DefaultFormatStyle,
                                         file,
                                         clang::format::DefaultFallbackStyle,
                                         "");
    if(!style) {
        return std::unexpected(std::format("{}", style.takeError()));
    }

    std::vector<tooling::Range> ranges = {range};
    auto include_replacements = clang::format::sortIncludes(*style, content, ranges, file);
    auto changed = tooling::applyAllReplacements(content, include_replacements);
    if(!changed) {
        return std::unexpected(std::format("{}", changed.takeError()));
    }

    return include_replacements.merge(clang::format::reformat(
        *style,
        *changed,
        tooling::calculateRangesAfterReplacements(include_replacements, ranges)));
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
        auto begin = replacement.getOffset();
        auto end = begin + replacement.getLength();
        protocol::TextEdit edit{
            .range = *map.to_range(begin, end),
            .new_text = replacement.getReplacementText().str(),
        };
        edits.push_back(std::move(edit));
    }

    return edits;
}

}  // namespace clice::feature
