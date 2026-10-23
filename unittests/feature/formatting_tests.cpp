module;

#include "support/prelude.h"

module clice;

import :feature.formatting;
import :unittests.utils;
import :unittests.utils.temp_dir;

namespace clice::testing {

namespace {

ZEST_SUITE(Formatting) {

ZEST_CASE(Simple) {
    auto edits = feature::document_format("main.cpp", "int main() { return 0; }");
    ZASSERT(edits.has_value());
    ZASSERT(!edits->empty());
}

ZEST_CASE(RangeFormat) {
    llvm::StringRef code = "int x=1;\nint   y =  2 ;\nint z=3;\n";
    LocalSourceRange range;
    range.begin = static_cast<std::uint32_t>(code.find("int   y"));
    range.end = static_cast<std::uint32_t>(code.find("\nint z") + 1);
    auto range_edits = feature::document_format("main.cpp", code, range);
    auto full_edits = feature::document_format("main.cpp", code);
    ZASSERT(range_edits.has_value());
    ZASSERT(full_edits.has_value());
    ZASSERT(!range_edits->empty());
    ZEXPECT(range_edits->size() <= full_edits->size());
}

ZEST_CASE(Idempotent) {
    llvm::StringRef code = "int main() {\n    return 0;\n}\n";
    auto edits = feature::document_format("main.cpp", code);
    ZASSERT(edits.has_value());
    ZEXPECT(edits->empty());
}

ZEST_CASE(IncludeSort) {
    llvm::StringRef code = "#include <vector>\n#include <algorithm>\n\nint main() {}\n";
    auto edits = feature::document_format("main.cpp", code);
    ZASSERT(edits.has_value());
    ZASSERT(!edits->empty());
}

ZEST_CASE(CRLFIncludeSort) {
    llvm::StringRef code = "#include \"b.h\"\r\n#include \"a.h\"\r\nint x;\r\n";
    auto edits = feature::document_format("main.cpp", code);
    ZASSERT(edits.has_value());
    ZASSERT(!edits->empty());

    std::string result = code.str();
    for(const auto& edit: views::reverse(*edits)) {
        result.replace(edit.range.begin, edit.range.length(), edit.text);
    }
    ZEXPECT(result == "#include \"a.h\"\r\n#include \"b.h\"\r\nint x;\r\n");
}

ZEST_CASE(InvalidStyleFile) {
    TempDir tmp;
    tmp.touch(".clang-format", "BasedOnStyle: LLVM\nIndentWdith: 4\n");
    auto edits = feature::document_format(tmp.path("main.cpp"), "int  x;\n");
    ZASSERT(!edits.has_value());
    ZEXPECT(llvm::StringRef(edits.error()).contains("unknown key 'IndentWdith'"));
}

ZEST_CASE(EditsNeedStyleFile) {
    TempDir tmp;
    auto file = tmp.path("main.cpp");
    llvm::StringRef code = "int f();\n";
    std::vector<TextReplacement> edits = {
        {{8, 8}, "int  g( ) {}\n"}
    };
    auto unchanged = feature::format_edits(file, code, edits);
    ZASSERT(unchanged.size() == 1U);
    ZEXPECT(unchanged[0].text == "int  g( ) {}\n");

    // A configuration inheriting from a parent that does not exist still
    // configures the file: its options land on the LLVM style.
    tmp.touch(".clang-format", "BasedOnStyle: InheritParentConfig\n");
    auto formatted = feature::format_edits(file, code, edits);
    std::string result = code.str();
    for(const auto& edit: views::reverse(formatted)) {
        result.replace(edit.range.begin, edit.range.length(), edit.text);
    }
    ZEXPECT(result == "int f();\nint g() {}\n");
}

};  // ZEST_SUITE(Formatting)

}  // namespace

}  // namespace clice::testing
