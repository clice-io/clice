module;

#include "modules/prelude.h"

module clice;

import :syntax.completion;
import :syntax.dependency_graph;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

ZEST_SUITE(FollowsAccessOperator) {

ZEST_CASE(MemberAccess) {
    ZEXPECT(follows_access_operator("w.", 2));
    ZEXPECT(follows_access_operator("p->", 3));
    ZEXPECT(follows_access_operator("std::", 5));
}

ZEST_CASE(PackEllipsis) {
    ZEXPECT(!follows_access_operator("template <typename..", 20));
    ZEXPECT(!follows_access_operator("template <typename...", 21));
}

ZEST_CASE(NumericLiteral) {
    ZEXPECT(!follows_access_operator("float f = 3.", 12));
    ZEXPECT(!follows_access_operator("x = 0x1F.", 9));
    ZEXPECT(follows_access_operator("x1.", 3));
    ZEXPECT(follows_access_operator("f(1).", 5));
}

ZEST_CASE(TemplateDelimiters) {
    ZEXPECT(!follows_access_operator("template<", 9));
    ZEXPECT(!follows_access_operator("template<typename T>", 20));
}

ZEST_CASE(PostfixDecrement) {
    ZEXPECT(!follows_access_operator("while (x-->", 11));
}

ZEST_CASE(NonAsciiOperand) {
    ZEXPECT(follows_access_operator("vé2.", 5));
}

ZEST_CASE(CursorBeforeOperator) {
    // Only the text up to the cursor counts.
    ZEXPECT(!follows_access_operator("w.x", 1));
}

};  // ZEST_SUITE(FollowsAccessOperator)

ZEST_SUITE(DetectCompletionContext) {

ZEST_CASE(IncludeAngled) {
    auto ctx = detect_completion_context("#include <vec", 13);
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "vec");
}

ZEST_CASE(IncludeQuoted) {
    auto ctx = detect_completion_context("#include \"my_header", 19);
    ZEXPECT(ctx.kind == CompletionContext::IncludeQuoted);
    ZEXPECT(ctx.prefix == "my_header");
}

ZEST_CASE(IncludeAngledWithSpaces) {
    auto ctx = detect_completion_context("  #  include  <sys/", 19);
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "sys/");
}

ZEST_CASE(IncludeEmpty) {
    auto ctx = detect_completion_context("#include <", 10);
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(CursorInsideKeyword) {
    // A keyword is only "typed" once the cursor passed its end.
    ZEXPECT(detect_completion_context("#include", 5).kind == CompletionContext::None);
    ZEXPECT(detect_completion_context("import", 3).kind == CompletionContext::None);
}

ZEST_CASE(CursorAtNewline) {
    // A cursor sitting on a line's terminating newline still completes the
    // statement that line opened.
    auto ctx = detect_completion_context("import std\nint x;", 10);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(CrlfLineEndings) {
    auto ctx = detect_completion_context("#include <a>\r\n#include <ve", 26);
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "ve");
}

ZEST_CASE(ImportSimple) {
    auto ctx = detect_completion_context("import std", 10);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(ExportImport) {
    auto ctx = detect_completion_context("export import my_mod", 20);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "my_mod");
}

ZEST_CASE(ImportWithSemicolon) {
    auto ctx = detect_completion_context("import st;\n", 9);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "st");
    ZEXPECT(detect_completion_context("import std;\n", 11).kind == CompletionContext::None);
}

ZEST_CASE(ImportEmpty) {
    auto ctx = detect_completion_context("import ", 7);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(NormalCode) {
    auto ctx = detect_completion_context("int main() {", 12);
    ZEXPECT(ctx.kind == CompletionContext::None);
}

ZEST_CASE(MultilineAtSecondLine) {
    std::string text = "#include <vector>\n#include <str";
    auto ctx = detect_completion_context(text, text.size());
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "str");
}

ZEST_CASE(NotImportKeyword) {
    auto ctx = detect_completion_context("importlib foo", 13);
    ZEXPECT(ctx.kind == CompletionContext::None);
}

ZEST_CASE(HashOnly) {
    auto ctx = detect_completion_context("#", 1);
    ZEXPECT(ctx.kind == CompletionContext::None);
}

ZEST_CASE(ImportDottedPrefix) {
    auto ctx = detect_completion_context("import std.io", 13);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "std.io");
}

ZEST_CASE(ImportPartitionPrefix) {
    auto ctx = detect_completion_context("import :core", 12);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == ":core");
}

ZEST_CASE(ImportPartitionEmpty) {
    auto ctx = detect_completion_context("import :", 8);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == ":");
}

ZEST_CASE(ImportWithLeadingSpaces) {
    auto ctx = detect_completion_context("  import std", 12);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(ExportImportEmpty) {
    auto ctx = detect_completion_context("export import ", 14);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(ImportAfterNewline) {
    std::string text = "module foo;\nimport ";
    auto ctx = detect_completion_context(text, text.size());
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(ImportCursorMidLine) {
    // The prefix is truncated at the cursor; trailing text is ignored.
    auto ctx = detect_completion_context("import std.io", 10);
    ZEXPECT(ctx.kind == CompletionContext::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(IncludeReplaceSpan) {
    // The last path component through its untyped rest, delimiter excluded.
    auto ctx = detect_completion_context("#include <sys/ty.h>", 16);
    ZEXPECT(ctx.prefix == "sys/ty");
    ZEXPECT(ctx.replace.begin == 14u);
    ZEXPECT(ctx.replace.end == 18u);
}

ZEST_CASE(IncludeNameWithSpaces) {
    auto closed = detect_completion_context("#include \"my header.h\"", 12);
    ZEXPECT(closed.replace.end == 21u);
    auto open = detect_completion_context("#include \"my header.h", 12);
    ZEXPECT(open.replace.end == 12u);
}

ZEST_CASE(ImportReplaceSpan) {
    auto ctx = detect_completion_context("import std.io", 10);
    ZEXPECT(ctx.replace.begin == 7u);
    ZEXPECT(ctx.replace.end == 13u);
}

ZEST_CASE(ImportMemberAccess) {
    ZEXPECT(detect_completion_context("import->x", 8).kind == CompletionContext::None);
}

};  // ZEST_SUITE(DetectCompletionContext)

ZEST_SUITE(CompleteModuleImport) {

ZEST_CASE(PrefixMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.net", Fid{3});
    modules.add_module("my_lib", Fid{4});

    auto results = complete_module_import(modules, "std", "");
    ZEXPECT(results.size() == 3u);
    for(auto& name: results) {
        ZEXPECT(name.starts_with("std"));
    }
}

ZEST_CASE(EmptyPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = complete_module_import(modules, "", "");
    ZEXPECT(results.size() == 2u);
}

ZEST_CASE(NoMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = complete_module_import(modules, "xyz", "");
    ZEXPECT(results.empty());
}

ZEST_CASE(EmptyModules) {
    clice::DependencyGraph modules;
    auto results = complete_module_import(modules, "std", "");
    ZEXPECT(results.empty());
}

ZEST_CASE(DottedPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.core", Fid{3});
    modules.add_module("boost.asio", Fid{4});

    auto results = complete_module_import(modules, "std.", "");
    ZEXPECT(results.size() == 2u);
    for(auto& name: results) {
        ZEXPECT(name.starts_with("std."));
    }
}

ZEST_CASE(PrefixIsFullName) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});

    auto results = complete_module_import(modules, "std", "");
    ZEXPECT(results.size() == 2u);
}

ZEST_CASE(OwnPartitionsOnly) {
    clice::DependencyGraph modules;
    modules.add_module("foo", Fid{1});
    modules.add_module("foo:core", Fid{2});
    modules.add_module("foo:utils", Fid{3});
    modules.add_module("bar", Fid{4});
    modules.add_module("bar:impl", Fid{5});

    auto names = complete_module_import(modules, "", "foo");
    ZEXPECT(names == std::vector<std::string>{":core", ":utils", "bar"});
    auto partitions = complete_module_import(modules, ":", "foo:utils");
    ZEXPECT(partitions == std::vector<std::string>{":core"});
    ZEXPECT(complete_module_import(modules, ":", "").empty());
    ZEXPECT(complete_module_import(modules, "foo:", "foo").empty());
}

};  // ZEST_SUITE(CompleteModuleImport)

}  // namespace
}  // namespace clice::testing
