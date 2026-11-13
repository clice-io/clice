module clice;

import :feature.selection_ranges;
import :syntax.lexer;
import :syntax.source_text;
import :unittests.utils;

namespace clice::testing {

namespace {

/// The text-only chain at `offset`, each step as the text it spans.
std::vector<std::string> steps_at(llvm::StringRef content, std::uint32_t offset) {
    SourceText text(content, dialect_of());
    auto chains = feature::lexical_selection_ranges(text, {offset});
    std::vector<std::string> steps;
    for(auto range: chains.front()) {
        steps.push_back(content.slice(range.begin, range.end).str());
    }
    return steps;
}

ZEST_SUITE(selection_ranges) {

/// The steps stop at 128, a depth no case's brackets reach.
ZEST_CASE(DeepNestingCapped) {
    std::string text = std::string(1000, '(') + "x" + std::string(1000, ')');
    auto steps = steps_at(text, 1000);
    ZEXPECT(steps.size() == 128U);
    ZEXPECT(steps.front() == "x");
    ZEXPECT(steps.back() == text);
}

};  // ZEST_SUITE(selection_ranges)

}  // namespace

}  // namespace clice::testing
