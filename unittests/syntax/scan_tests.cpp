module;

#include "modules/prelude.h"

module clice;

import :syntax.scan;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(Scan) {

// === scan_quick() — include and conditional extraction; module
// declaration coverage lives in module_scan_tests.cpp ===

ZEST_CASE(BasicIncludes) {
    auto result = scan_quick(R"(
#include <vector>
#include "foo/bar.h"
int x = 1;
)");

    ZASSERT(result.includes.size() == 2u);
    ZEXPECT(result.includes[0].path == "vector");
    ZEXPECT(result.includes[0].is_angled);
    ZEXPECT(!result.includes[0].conditional);
    ZEXPECT(result.includes[1].path == "foo/bar.h");
    ZEXPECT(!result.includes[1].is_angled);
    ZEXPECT(!result.includes[1].conditional);
    ZEXPECT(result.module_name.empty());
    ZEXPECT(!result.has_import);
}

ZEST_CASE(ImportDetected) {
    // Detection only: the names stay uncollected (imports macro-expand,
    // so lexical text cannot name edges) — the flag marks the file worth
    // a precise scan.
    auto result = scan_quick(R"(
import some.mod;
int x = 1;
)");
    ZEXPECT(result.has_import);
    ZEXPECT(result.modules.empty());

    auto exported = scan_quick(R"(
export module top;
export import :part;
)");
    ZEXPECT(exported.has_import);

    // A conditional module declaration defers the name to the
    // preprocessor fallback but must not hide later directives.
    auto conditional = scan_quick(R"(
#if 0
export module maybe;
#endif
import m;
)");
    ZEXPECT(conditional.need_preprocess);
    ZEXPECT(conditional.has_import);
}

ZEST_CASE(IncludeOffsets) {
    llvm::StringRef content = R"(int x;
#include <a.h>
#include "b.h"
)";
    auto result = scan_quick(content);

    ZASSERT(result.includes.size() == 2u);
    ZEXPECT(result.includes[0].offset == static_cast<std::uint32_t>(content.find("#include <a")));
    ZEXPECT(result.includes[1].offset ==
            static_cast<std::uint32_t>(content.find(R"(#include "b)")));
}

ZEST_CASE(IndentedIncludeOffset) {
    llvm::StringRef content = "  #  include <a.h>\n";
    auto result = scan_quick(content);

    ZASSERT(result.includes.size() == 1u);
    // The offset points at the `#`, not the line start.
    ZEXPECT(result.includes[0].offset == 2u);
}

ZEST_CASE(ConditionalIncludes) {
    auto result = scan_quick(R"(
#include <always.h>
#ifdef FOO
#include <conditional.h>
#endif
#include <after.h>
)");

    ZASSERT(result.includes.size() == 3u);
    ZEXPECT(result.includes[0].path == "always.h");
    ZEXPECT(!result.includes[0].conditional);
    ZEXPECT(result.includes[1].path == "conditional.h");
    ZEXPECT(result.includes[1].conditional);
    ZEXPECT(result.includes[2].path == "after.h");
    ZEXPECT(!result.includes[2].conditional);
}

ZEST_CASE(NestedConditionals) {
    auto result = scan_quick(R"(
#ifdef A
#ifdef B
#include <nested.h>
#endif
#include <outer.h>
#endif
#include <top.h>
)");

    ZASSERT(result.includes.size() == 3u);
    ZEXPECT(result.includes[0].path == "nested.h");
    ZEXPECT(result.includes[0].conditional);
    ZEXPECT(result.includes[1].path == "outer.h");
    ZEXPECT(result.includes[1].conditional);
    ZEXPECT(result.includes[2].path == "top.h");
    ZEXPECT(!result.includes[2].conditional);
}

ZEST_CASE(ElifBranchInclude) {
    auto result = scan_quick(R"(
#if defined(A)
#include <a.h>
#elif defined(B)
#include <b.h>
#else
#include <c.h>
#endif
#include <after.h>
)");

    ZASSERT(result.includes.size() == 4u);
    for(std::size_t i = 0; i < 3; i += 1) {
        ZEXPECT(result.includes[i].conditional);
        ZEXPECT(result.includes[i].conditional_depth == 1);
    }
    ZEXPECT(!result.includes[3].conditional);
}

ZEST_CASE(IncludeNext) {
    auto result = scan_quick(R"(
#include <normal.h>
#include_next <chained.h>
)");

    ZASSERT(result.includes.size() == 2u);
    ZEXPECT(!result.includes[0].is_include_next);
    ZEXPECT(result.includes[1].is_include_next);
    ZEXPECT(result.includes[1].path == "chained.h");
}

ZEST_CASE(DirectivesHashIgnoresBody) {
    // The hash follows the directive lines only: the text a precise scan
    // reads of the file.
    auto hash = [](llvm::StringRef text) {
        return scan_quick(text).directives_hash;
    };
    auto base = hash("import m;\n#define F(x) x\nint f() { return 1; }\n");
    ZEXPECT(base == hash("import m;\n#define F(x) x\nint f() { return 2; }\n"));
    ZEXPECT(base != hash("import n;\n#define F(x) x\nint f() { return 1; }\n"));
    ZEXPECT(base != hash("import m;\n#define F (x) x\nint f() { return 1; }\n"));
}

ZEST_CASE(EmptyContent) {
    auto result = scan_quick("");
    ZEXPECT(result.includes.empty());
    ZEXPECT(result.module_name.empty());
    ZEXPECT(!result.need_preprocess);
}

ZEST_CASE(NoDirectives) {
    auto result = scan_quick(R"(
int main() {
    return 0;
}
)");

    ZEXPECT(result.includes.empty());
    ZEXPECT(result.module_name.empty());
    ZEXPECT(!result.is_implementation_unit());
    ZEXPECT(!result.need_preprocess);
}

// === scan_precise() tests ===

ZEST_CASE(PreciseWithContent) {
    // The content replaces the file on disk, even when empty.
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    auto empty_path = TestVFS::path("empty.cpp");
    vfs->add("main.cpp", R"(#include "header.h")");
    vfs->add("empty.cpp");
    vfs->add("header.h", "import dep;");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    ZEXPECT(scan_precise(args, TestVFS::root(), {}, vfs).modules.size() == 1u);
    ZEXPECT(scan_precise(args, TestVFS::root(), llvm::StringRef(), vfs).modules.empty());

    args.back() = empty_path.c_str();
    auto remapped = scan_precise(args, TestVFS::root(), R"(#include "header.h")", vfs);
    ZASSERT(remapped.modules.size() == 1u);
    ZEXPECT(remapped.modules[0] == "dep");
}

ZEST_CASE(PreciseHonorsWorkingDirectory) {
    // The scan interprets the command like the compile does: an explicit
    // -working-directory wins over the entry's directory, and a relative
    // one resolves from it — else a header the compile finds through a
    // relative search path scans as missing and its imports go unseen.
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp", R"(#include "x.h")");
    vfs->add("other/rel/x.h", "import dep;");

    auto absolute = "-working-directory=" + TestVFS::path("other");
    auto args = std::vector<const char*>{"clang++",
                                         "-std=c++20",
                                         absolute.c_str(),
                                         "-Irel",
                                         main_path.c_str()};
    ZEXPECT(scan_precise(args, TestVFS::root(), {}, vfs).modules.size() == 1u);

    args[2] = "-working-directory=other";
    ZEXPECT(scan_precise(args, TestVFS::root(), {}, vfs).modules.size() == 1u);
}

ZEST_CASE(DirectiveCutAtEnd) {
    // Typing leaves an `#if(` cut off at the end of the buffer; behind a
    // forced include, clang's directives lexer crashed on it.
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    auto forced = TestVFS::path("empty.h");
    vfs->add("main.cpp");
    vfs->add("empty.h");

    auto args = std::vector<const char*>{"clang++",
                                         "-std=c++20",
                                         "-include",
                                         forced.c_str(),
                                         main_path.c_str()};
    auto result =
        scan_precise(args,
                     TestVFS::root(),
                     llvm::StringRef("#define VERSION_CODE()\r\n  #if(MSVC)VERSION_CODE("),
                     vfs);
    ZEXPECT(result.modules.empty());
}

};  // ZEST_SUITE(Scan)

ZEST_SUITE(PreambleBound) {

ZEST_CASE(Empty) {
    ZEXPECT(compute_preamble_bound("") == 0u);
}

ZEST_CASE(NoDirectives) {
    ZEXPECT(compute_preamble_bound("int x = 1;") == 0u);
}

ZEST_CASE(SingleInclude) {
    llvm::StringRef src = R"(
#include <vector>
int x;
)";
    auto bound = compute_preamble_bound(src);
    ZEXPECT(bound > 0u);
    ZEXPECT(bound <= src.find("int"));
}

ZEST_CASE(MultipleDirectives) {
    llvm::StringRef src = R"(
#include <vector>
#include <string>
#define FOO 1
int x;
)";
    auto bound = compute_preamble_bound(src);
    ZEXPECT(bound > src.find("#define"));
}

ZEST_CASE(GlobalModuleFragment) {
    llvm::StringRef src = R"(
module;
#include <vector>
export module foo;
)";
    auto bound = compute_preamble_bound(src);
    ZEXPECT(bound > 0u);
    ZEXPECT(bound < src.size());
}

ZEST_CASE(BoundAfterLastDirective) {
    llvm::StringRef src = R"(
#include <a>
#include <b>
int x;
)";
    ZEXPECT(compute_preamble_bound(src) == src.find("<b>") + 3);
}

ZEST_CASE(BoundWithModuleFragment) {
    llvm::StringRef src = R"(
module;
#include <a>
#include <b>
export module foo;
)";
    ZEXPECT(compute_preamble_bound(src) == src.find("<b>") + 3);

    llvm::StringRef marker_only = R"(module;
export module foo;
)";
    ZEXPECT(compute_preamble_bound(marker_only) == marker_only.find(';') + 1);
}

ZEST_CASE(StopsAtCode) {
    llvm::StringRef src = R"(
#include <a>
int x;
#include <b>
)";
    ZEXPECT(compute_preamble_bound(src) == src.find("<a>") + 3);
}

ZEST_CASE(ConditionalDirectives) {
    llvm::StringRef src = R"(
#ifndef GUARD
#define GUARD
#include <a>
#endif
int x;
)";
    auto bound = compute_preamble_bound(src);
    ZEXPECT(bound > src.find("#endif"));
}

};  // ZEST_SUITE(PreambleBound)

ZEST_SUITE(PreambleComplete) {

ZEST_CASE(CompleteQuotedInclude) {
    llvm::StringRef content = "#include \"foo.h\"\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(CompleteAngledInclude) {
    llvm::StringRef content = "#include <vector>\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(IncompleteQuotedInclude) {
    llvm::StringRef content = "#include \"foo\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(IncompleteAngledInclude) {
    llvm::StringRef content = "#include <sys/\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(IncludeWithNoPath) {
    llvm::StringRef content = "#include \nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(IncompleteEmbed) {
    llvm::StringRef content = "#embed <data\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(CompleteEmbed) {
    llvm::StringRef content = "#embed \"data.bin\" limit(4)\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(IncludeMacroUsage) {
    llvm::StringRef content = "#include FOO\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(MultipleIncludesAllComplete) {
    llvm::StringRef content = "#include <vector>\n#include \"foo.h\"\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(MultipleIncludesLastIncomplete) {
    llvm::StringRef content = "#include <vector>\n#include \"foo\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == content.find('\n'));
}

ZEST_CASE(IncompleteIncludeMidBlock) {
    // Typed in the middle of the block: the finished includes before it
    // stay in the preamble, the ones after it go with the body.
    llvm::StringRef content = "#include <a>\n#include <vec\n#include \"c.h\"\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == content.find('\n'));
}

// compute_preamble_bound does not include import/export lines in its
// bound, so we pass manual bounds covering the relevant lines.

ZEST_CASE(CompleteImport) {
    llvm::StringRef content = "import std;\nint x;";
    // Bound covers "import std;\n".
    ZEXPECT(complete_preamble_bound(content, 12) == 12);
}

ZEST_CASE(ImportMissingSemicolon) {
    llvm::StringRef content = "import std\nint x;";
    // Bound covers "import std\n".
    ZEXPECT(complete_preamble_bound(content, 11) == 0u);
}

ZEST_CASE(ImportWithNothing) {
    llvm::StringRef content = "import \nint x;";
    // Bound covers "import \n".
    ZEXPECT(complete_preamble_bound(content, 8) == 0u);
}

ZEST_CASE(CompleteExportModule) {
    llvm::StringRef content = "export module foo;\nint x;";
    // Bound covers "export module foo;\n".
    ZEXPECT(complete_preamble_bound(content, 19) == 19);
}

ZEST_CASE(ExportModuleMissingSemicolon) {
    llvm::StringRef content = "export module foo\nint x;";
    // Bound covers "export module foo\n".
    ZEXPECT(complete_preamble_bound(content, 18) == 0u);
}

ZEST_CASE(SplicedIncompleteInclude) {
    // The unterminated filename token's spelling begins with the splice.
    llvm::StringRef content = "#include \\\n<foo\nint x;";
    ZEXPECT(complete_preamble_bound(content, 16) == 0u);
}

ZEST_CASE(SplicedCompleteInclude) {
    llvm::StringRef content = "#include \\\n<foo.h>\nint x;";
    ZEXPECT(complete_preamble_bound(content, 19) == 19);
}

ZEST_CASE(AngledHashImport) {
    llvm::StringRef content = "#import <foo.h>\nint x;";
    ZEXPECT(complete_preamble_bound(content, 16) == 16);
}

ZEST_CASE(TrailingCommentAfterSemicolon) {
    // A trailing comment must not hide the terminating semicolon.
    llvm::StringRef content = "import std; // done\nint x;";
    ZEXPECT(complete_preamble_bound(content, 20) == 20);
}

ZEST_CASE(TrailingCommentNoSemicolon) {
    llvm::StringRef content = "import std // ;\nint x;";
    // The semicolon inside the comment does not terminate the statement.
    ZEXPECT(complete_preamble_bound(content, 16) == 0u);
}

ZEST_CASE(CompleteExportImport) {
    llvm::StringRef content = "export import std;\nint x;";
    // Bound covers "export import std;\n".
    ZEXPECT(complete_preamble_bound(content, 19) == 19);
}

ZEST_CASE(EmptyPreamble) {
    llvm::StringRef content = "int x;";
    ZEXPECT(complete_preamble_bound(content, 0) == 0);
}

ZEST_CASE(NonImportIncludeLinesIgnored) {
    llvm::StringRef content = "#define FOO 1\n#ifdef BAR\n#endif\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(ImportantDoesNotMatchImport) {
    // "important" starts with "import" but should NOT be treated as an import.
    llvm::StringRef content = "#include <vector>\nint x;";
    auto bound = compute_preamble_bound(content);
    // Manually test with content that has "important" within the preamble region.
    // Since compute_preamble_bound won't include non-directive lines, we test
    // complete_preamble_bound directly with a crafted bound.
    llvm::StringRef crafted = "important = 1;\n";
    ZEXPECT(complete_preamble_bound(crafted, crafted.size()) == crafted.size());
}

ZEST_CASE(PreprocessorDirectivesIgnored) {
    llvm::StringRef content = "#ifdef FOO\n#define BAR 1\n#endif\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

ZEST_CASE(MixedIncludeAndImportAllComplete) {
    llvm::StringRef content = "#include <vector>\nimport std;\nint x;";
    auto bound = compute_preamble_bound(content);
    ZEXPECT(complete_preamble_bound(content, bound) == bound);
}

};  // ZEST_SUITE(PreambleComplete)

ZEST_SUITE(PreambleUsesMacros) {

ZEST_CASE(Includes) {
    llvm::StringRef content = "#include <a>\n#include_next \"b.h\"\n#pragma message(\"m\")\nint x;";
    ZEXPECT(!preamble_uses_macros(content, compute_preamble_bound(content)));
}

ZEST_CASE(Definition) {
    llvm::StringRef content = "#include <a>\n#define A 1\nint x;";
    ZEXPECT(preamble_uses_macros(content, compute_preamble_bound(content)));
}

ZEST_CASE(Condition) {
    llvm::StringRef content = "#ifdef A\n#include <a>\n#endif\nint x;";
    ZEXPECT(preamble_uses_macros(content, compute_preamble_bound(content)));
}

ZEST_CASE(DefinitionPastBound) {
    llvm::StringRef content = "#include <a>\nint x;\n#define A 1\n";
    ZEXPECT(!preamble_uses_macros(content, compute_preamble_bound(content)));
}

};  // ZEST_SUITE(PreambleUsesMacros)

}  // namespace
}  // namespace clice::testing
