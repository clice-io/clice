module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :syntax.position;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;

namespace clice::testing {

namespace {

ZEST_SUITE(Formatting) {

ZEST_CASE(Simple) {
    auto edits = feature::document_format("main.cpp", "int main() { return 0; }", std::nullopt);
    ZASSERT(edits.size() != 0U);
}

ZEST_CASE(RangeFormat) {
    llvm::StringRef code = "int x=1;\nint   y =  2 ;\nint z=3;\n";
    LocalSourceRange range;
    range.begin = static_cast<std::uint32_t>(code.find("int   y"));
    range.end = static_cast<std::uint32_t>(code.find("\nint z") + 1);
    auto range_edits = feature::document_format("main.cpp", code, range);
    auto full_edits = feature::document_format("main.cpp", code, std::nullopt);
    ZASSERT(range_edits.size() != 0U);
    ZEXPECT(range_edits.size() <= full_edits.size());
}

ZEST_CASE(Idempotent) {
    llvm::StringRef code = "int main() {\n    return 0;\n}\n";
    auto edits = feature::document_format("main.cpp", code, std::nullopt);
    ZEXPECT(edits.size() == 0U);
}

ZEST_CASE(IncludeSort) {
    llvm::StringRef code = "#include <vector>\n#include <algorithm>\n\nint main() {}\n";
    auto edits = feature::document_format("main.cpp", code, std::nullopt);
    ZASSERT(edits.size() != 0U);
}

ZEST_CASE(CRLFIncludeSort) {
    llvm::StringRef code = "#include \"b.h\"\r\n#include \"a.h\"\r\nint x;\r\n";
    auto edits = feature::document_format("main.cpp", code, std::nullopt);
    ZASSERT(edits.size() != 0U);

    auto lines = kota::ipc::lsp::line_starts(code);
    PositionMap map(code, lines);
    std::string result = code.str();
    for(const auto& edit: llvm::reverse(edits)) {
        auto range = map.offset_range(edit.range, PositionEncoding::UTF16);
        result.replace(range.begin, range.length(), edit.new_text);
    }
    ZEXPECT(result == "#include \"a.h\"\r\n#include \"b.h\"\r\nint x;\r\n");
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
    for(const auto& edit: llvm::reverse(formatted)) {
        result.replace(edit.range.begin, edit.range.length(), edit.text);
    }
    ZEXPECT(result == "int f();\nint g() {}\n");
}

};  // ZEST_SUITE(Formatting)

}  // namespace

}  // namespace clice::testing
