module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :syntax.lexer;
import :unittests.utils;

namespace clice::testing {

namespace {

/// The text-only chain at `offset`, each step as the text it spans.
std::vector<std::string> steps_at(llvm::StringRef content, std::uint32_t offset) {
    auto chains = feature::lexical_selection_ranges(content, dialect_of(), {offset});
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
    for(llvm::StringRef text: {R"(f("open)", R"(R"x(open)", R"(R"open)", "u8'"}) {
        ZEXPECT(steps_at(text, text.size() - 1) == std::vector<std::string>{""});
    }
    // A comment left open runs to the end of the file.
    llvm::StringRef comment = "int x; /* open";
    ZEXPECT(steps_at(comment, comment.size() - 1) == std::vector<std::string>{"open", "/* open"});
}

ZEST_CASE(StrayClosers) {
    ZEXPECT(steps_at("} f(a) ]", 4) == std::vector<std::string>{"a", "(a)"});
}

ZEST_CASE(DeepNestingCapped) {
    std::string text = std::string(1000, '(') + "x" + std::string(1000, ')');
    auto steps = steps_at(text, 1000);
    ZEXPECT(steps.size() == 128U);
    ZEXPECT(steps.front() == "x");
    ZEXPECT(steps.back() == text);
}

};  // ZEST_SUITE(SelectionRange)

}  // namespace

}  // namespace clice::testing
