#include "test/test.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

TEST_SUITE(Scan) {

// === scan() tests ===

TEST_CASE(BasicIncludes) {
    auto result = scan(R"(
#include <vector>
#include "foo/bar.h"
int x = 1;
)");

    ASSERT_EQ(result.includes.size(), 2u);
    EXPECT_EQ(result.includes[0].path, "vector");
    EXPECT_TRUE(result.includes[0].is_angled);
    EXPECT_FALSE(result.includes[0].conditional);
    EXPECT_EQ(result.includes[1].path, "foo/bar.h");
    EXPECT_FALSE(result.includes[1].is_angled);
    EXPECT_FALSE(result.includes[1].conditional);
    EXPECT_TRUE(result.module_name.empty());
}

TEST_CASE(ConditionalIncludes) {
    auto result = scan(R"(
#include <always.h>
#ifdef FOO
#include <conditional.h>
#endif
#include <after.h>
)");

    ASSERT_EQ(result.includes.size(), 3u);
    EXPECT_EQ(result.includes[0].path, "always.h");
    EXPECT_FALSE(result.includes[0].conditional);
    EXPECT_EQ(result.includes[1].path, "conditional.h");
    EXPECT_TRUE(result.includes[1].conditional);
    EXPECT_EQ(result.includes[2].path, "after.h");
    EXPECT_FALSE(result.includes[2].conditional);
}

TEST_CASE(NestedConditionals) {
    auto result = scan(R"(
#ifdef A
#ifdef B
#include <nested.h>
#endif
#include <outer.h>
#endif
#include <top.h>
)");

    ASSERT_EQ(result.includes.size(), 3u);
    EXPECT_EQ(result.includes[0].path, "nested.h");
    EXPECT_TRUE(result.includes[0].conditional);
    EXPECT_EQ(result.includes[1].path, "outer.h");
    EXPECT_TRUE(result.includes[1].conditional);
    EXPECT_EQ(result.includes[2].path, "top.h");
    EXPECT_FALSE(result.includes[2].conditional);
}

TEST_CASE(ModuleDeclaration) {
    auto result = scan(R"(
module;
#include <header.h>
export module my.module;
)");

    EXPECT_EQ(result.module_name, "my.module");
    EXPECT_TRUE(result.is_interface_unit);
    EXPECT_FALSE(result.need_preprocess);
    ASSERT_EQ(result.includes.size(), 1u);
    EXPECT_EQ(result.includes[0].path, "header.h");
    EXPECT_TRUE(result.includes[0].is_angled);
}

TEST_CASE(ModulePartition) {
    auto result = scan(R"(
module my.module:part;
)");

    EXPECT_EQ(result.module_name, "my.module:part");
    EXPECT_FALSE(result.is_interface_unit);
}

TEST_CASE(ModuleImplementation) {
    auto result = scan(R"(
module my.module;
)");

    EXPECT_EQ(result.module_name, "my.module");
    EXPECT_FALSE(result.is_interface_unit);
}

TEST_CASE(ConditionalModule) {
    auto result = scan(R"(
#ifdef USE_MODULES
export module foo;
#endif
)");

    EXPECT_TRUE(result.module_name.empty());
    EXPECT_TRUE(result.need_preprocess);
}

TEST_CASE(GlobalModuleFragment) {
    auto result = scan(R"(
module;
export module test;
)");

    EXPECT_EQ(result.module_name, "test");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(EmptyContent) {
    auto result = scan("");
    EXPECT_TRUE(result.includes.empty());
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.need_preprocess);
}

TEST_CASE(NoDirectives) {
    auto result = scan(R"(
int main() {
    return 0;
}
)");

    EXPECT_TRUE(result.includes.empty());
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.is_interface_unit);
    EXPECT_FALSE(result.need_preprocess);
}

// === scan_precise() tests ===

TEST_CASE(PreciseBasic) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp", R"(
#include "header.h"
int main() {}
)");
    vfs->add("header.h", R"(
#pragma once
int x = 1;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    ASSERT_EQ(result.includes.size(), 1u);
    EXPECT_FALSE(result.includes[0].not_found);
    EXPECT_FALSE(result.includes[0].conditional);
}

TEST_CASE(PreciseConditionalWithDefine) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp", R"(
#define USE_FOO
#ifdef USE_FOO
#include "foo.h"
#endif
#ifndef USE_FOO
#include "bar.h"
#endif
)");
    vfs->add("foo.h");
    vfs->add("bar.h");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    // Precise mode evaluates conditionals: only foo.h should be included.
    ASSERT_EQ(result.includes.size(), 1u);
    EXPECT_TRUE(result.includes[0].conditional);
    EXPECT_TRUE(result.includes[0].path.find("foo.h") != std::string::npos);
}

TEST_CASE(PreciseWithContent) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp");
    vfs->add("header.h");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), R"(#include "header.h")", nullptr, vfs);

    ASSERT_EQ(result.includes.size(), 1u);
    EXPECT_FALSE(result.includes[0].not_found);
}

};  // TEST_SUITE(Scan)

TEST_SUITE(PreambleBound) {

TEST_CASE(Empty) {
    EXPECT_EQ(compute_preamble_bound(""), 0u);
}

TEST_CASE(NoDirectives) {
    EXPECT_EQ(compute_preamble_bound("int x = 1;"), 0u);
}

TEST_CASE(SingleInclude) {
    llvm::StringRef src = R"(
#include <vector>
int x;
)";
    auto bound = compute_preamble_bound(src);
    EXPECT_TRUE(bound > 0u);
    EXPECT_TRUE(bound <= src.find("int"));
}

TEST_CASE(MultipleDirectives) {
    llvm::StringRef src = R"(
#include <vector>
#include <string>
#define FOO 1
int x;
)";
    auto bound = compute_preamble_bound(src);
    EXPECT_TRUE(bound > src.find("#define"));
}

TEST_CASE(GlobalModuleFragment) {
    llvm::StringRef src = R"(
module;
#include <vector>
export module foo;
)";
    auto bound = compute_preamble_bound(src);
    EXPECT_TRUE(bound > 0u);
    EXPECT_TRUE(bound < src.size());
}

TEST_CASE(BoundsVector) {
    llvm::StringRef src = R"(
#include <a>
#include <b>
int x;
)";
    auto bounds = compute_preamble_bounds(src);
    ASSERT_EQ(bounds.size(), 2u);
    EXPECT_TRUE(bounds[0] < bounds[1]);
}

TEST_CASE(BoundsWithModuleFragment) {
    llvm::StringRef src = R"(
module;
#include <a>
#include <b>
export module foo;
)";
    auto bounds = compute_preamble_bounds(src);
    // module; + two #include = 3 bounds.
    ASSERT_EQ(bounds.size(), 3u);
    EXPECT_TRUE(bounds[0] < bounds[1]);
    EXPECT_TRUE(bounds[1] < bounds[2]);
}

TEST_CASE(StopsAtCode) {
    llvm::StringRef src = R"(
#include <a>
int x;
#include <b>
)";
    auto bounds = compute_preamble_bounds(src);
    ASSERT_EQ(bounds.size(), 1u);
}

TEST_CASE(ConditionalDirectives) {
    llvm::StringRef src = R"(
#ifndef GUARD
#define GUARD
#include <a>
#endif
int x;
)";
    auto bound = compute_preamble_bound(src);
    EXPECT_TRUE(bound > src.find("#endif"));
}

};  // TEST_SUITE(PreambleBound)

TEST_SUITE(PreambleComplete) {

// --- #include completeness ---

TEST_CASE(CompleteQuotedInclude) {
    llvm::StringRef content = "#include \"foo.h\"\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

TEST_CASE(CompleteAngledInclude) {
    llvm::StringRef content = "#include <vector>\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

TEST_CASE(IncompleteQuotedInclude) {
    llvm::StringRef content = "#include \"foo\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_FALSE(is_preamble_complete(content, bound));
}

TEST_CASE(IncompleteAngledInclude) {
    llvm::StringRef content = "#include <sys/\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_FALSE(is_preamble_complete(content, bound));
}

TEST_CASE(IncludeWithNoPath) {
    llvm::StringRef content = "#include \nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_FALSE(is_preamble_complete(content, bound));
}

TEST_CASE(IncludeMacroUsage) {
    llvm::StringRef content = "#include FOO\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

TEST_CASE(MultipleIncludesAllComplete) {
    llvm::StringRef content = "#include <vector>\n#include \"foo.h\"\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

TEST_CASE(MultipleIncludesLastIncomplete) {
    llvm::StringRef content = "#include <vector>\n#include \"foo\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_FALSE(is_preamble_complete(content, bound));
}

// --- C++20 module statements ---
// Note: compute_preamble_bound does not include import/export lines in its
// bound, so we pass manual bounds covering the relevant lines.

TEST_CASE(CompleteImport) {
    llvm::StringRef content = "import std;\nint x;";
    // Bound covers "import std;\n".
    EXPECT_TRUE(is_preamble_complete(content, 12));
}

TEST_CASE(ImportMissingSemicolon) {
    llvm::StringRef content = "import std\nint x;";
    // Bound covers "import std\n".
    EXPECT_FALSE(is_preamble_complete(content, 11));
}

TEST_CASE(ImportWithNothing) {
    llvm::StringRef content = "import \nint x;";
    // Bound covers "import \n".
    EXPECT_FALSE(is_preamble_complete(content, 8));
}

TEST_CASE(CompleteExportModule) {
    llvm::StringRef content = "export module foo;\nint x;";
    // Bound covers "export module foo;\n".
    EXPECT_TRUE(is_preamble_complete(content, 19));
}

TEST_CASE(ExportModuleMissingSemicolon) {
    llvm::StringRef content = "export module foo\nint x;";
    // Bound covers "export module foo\n".
    EXPECT_FALSE(is_preamble_complete(content, 18));
}

TEST_CASE(CompleteExportImport) {
    llvm::StringRef content = "export import std;\nint x;";
    // Bound covers "export import std;\n".
    EXPECT_TRUE(is_preamble_complete(content, 19));
}

// --- Edge cases ---

TEST_CASE(EmptyPreamble) {
    llvm::StringRef content = "int x;";
    EXPECT_TRUE(is_preamble_complete(content, 0));
}

TEST_CASE(NonImportIncludeLinesIgnored) {
    llvm::StringRef content = "#define FOO 1\n#ifdef BAR\n#endif\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

TEST_CASE(ImportantDoesNotMatchImport) {
    // "important" starts with "import" but should NOT be treated as an import.
    llvm::StringRef content = "#include <vector>\nint x;";
    auto bound = compute_preamble_bound(content);
    // Manually test with content that has "important" within the preamble region.
    // Since compute_preamble_bound won't include non-directive lines, we test
    // is_preamble_complete directly with a crafted bound.
    llvm::StringRef crafted = "important = 1;\n";
    EXPECT_TRUE(is_preamble_complete(crafted, crafted.size()));
}

TEST_CASE(PreprocessorDirectivesIgnored) {
    llvm::StringRef content = "#ifdef FOO\n#define BAR 1\n#endif\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

TEST_CASE(MixedIncludeAndImportAllComplete) {
    llvm::StringRef content = "#include <vector>\nimport std;\nint x;";
    auto bound = compute_preamble_bound(content);
    EXPECT_TRUE(is_preamble_complete(content, bound));
}

};  // TEST_SUITE(PreambleComplete)

}  // namespace
}  // namespace clice::testing
