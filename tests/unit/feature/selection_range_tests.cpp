module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :tests.unit.test.test;

namespace clice::testing {

namespace {

/// The text-only chain at `offset`, each step as the text it spans.
std::vector<std::string> steps_at(llvm::StringRef content, std::uint32_t offset) {
    auto chains = feature::lexical_selection_ranges(content,
                                                    feature::index_lang_options("", false),
                                                    {offset});
    std::vector<std::string> steps;
    for(auto range: chains.front()) {
        steps.push_back(content.slice(range.begin, range.end).str());
    }
    return steps;
}

ZEST_SUITE(SelectionRange) {

ZEST_CASE(EmptyText) {
    ZEXPECT(steps_at("", 0) == std::vector<std::string>{""});
}

ZEST_CASE(UnterminatedLiterals) {
    for(llvm::StringRef text: {R"(f("open)", R"(R"x(open)", R"(R"open)", "u8'", "int x; /* open"}) {
        ZEXPECT(steps_at(text, text.size() - 1) == std::vector<std::string>{""});
    }
}

ZEST_CASE(StrayClosers) {
    ZEXPECT(steps_at("} f(a) ]", 4) == std::vector<std::string>{"a", "(a)"});
}

};  // ZEST_SUITE(SelectionRange)

}  // namespace

}  // namespace clice::testing
