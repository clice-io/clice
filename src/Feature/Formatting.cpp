#include "Feature/Formatting.h"
#include "Support/Logging.h"
#include "Server/Convert.h"
#include "clang/Format/Format.h"

namespace clice::feature {

namespace tooling = clang::tooling;

static auto format(llvm::StringRef file, llvm::StringRef content, tooling::Range range)
    -> std::expected<tooling::Replacements, std::string> {
    /// Set the code to empty to avoid meaningless file type guess.
    /// See https://github.com/llvm/llvm-project/issues/48863.
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

std::vector<proto::TextEdit> document_format(llvm::StringRef file,
                                             llvm::StringRef content,
                                             std::optional<LocalSourceRange> range) {
    std::vector<proto::TextEdit> edits;

    auto selection =
        range ? tooling::Range(range->begin, range->length()) : tooling::Range(0, content.size());
    auto replacements = format(file, content, selection);
    if(!replacements) {
        logging::info("Fail to format for {}\n{}", file, replacements.error());
        return edits;
    }

    for(auto& replacement: *replacements) {
        proto::TextEdit edit;
        PositionConverter converter(content, PositionEncodingKind::UTF8);
        edit.range.start = converter.toPosition(replacement.getOffset());
        edit.range.end = converter.toPosition(replacement.getOffset() + replacement.getLength());
        edit.newText = replacement.getReplacementText();
        edits.emplace_back(std::move(edit));
    }

    return edits;
}

}  // namespace clice::feature
