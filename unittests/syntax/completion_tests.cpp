module;

#include "modules/prelude.h"

module clice;

import :syntax.completion;
import :syntax.dependency_graph;
import :syntax.lexer;
import :syntax.scan;
import :unittests.utils;

namespace clice::testing {
namespace {

bool follows_access_operator(llvm::StringRef text, std::uint32_t offset) {
    return clice::follows_access_operator(text, offset, dialect_of());
}

std::vector<WrittenDesignator> written_designators(llvm::StringRef text, std::uint32_t offset) {
    return clice::written_designators(text, offset, dialect_of());
}

PreambleCompletionContext detect_completion_context(llvm::StringRef text, std::uint32_t offset) {
    return clice::detect_completion_context(text, offset, dialect_of());
}

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

ZEST_CASE(StringsAndComments) {
    ZEXPECT(!follows_access_operator(R"(auto s = "a.)", 12));
    ZEXPECT(!follows_access_operator("int a; // a.", 12));
}

ZEST_CASE(CursorBeforeOperator) {
    // Only the text up to the cursor counts.
    ZEXPECT(!follows_access_operator("w.x", 1));
}

};  // ZEST_SUITE(FollowsAccessOperator)

ZEST_SUITE(WrittenDesignators) {

auto designators(llvm::StringRef text, std::size_t offset = llvm::StringRef::npos) {
    std::vector<std::string> names;
    for(auto designator: written_designators(text, std::min(offset, text.size()))) {
        names.push_back(designator.field.str());
    }
    return names;
}

auto whole(llvm::StringRef text) {
    std::vector<std::string> names;
    for(auto designator: written_designators(text, text.size())) {
        if(designator.whole) {
            names.push_back(designator.field.str());
        }
    }
    return names;
}

ZEST_CASE(EarlierFields) {
    ZEXPECT(designators("Size s = { .width = 800, .height{600}, .") ==
            std::vector<std::string>{"width", "height"});
}

ZEST_CASE(NestedBraces) {
    ZEXPECT(designators("Outer o = { .inner = { .x = 1, .") == std::vector<std::string>{"x"});
    ZEXPECT(designators("Outer o = { .inner = { .x = 1 }, .") == std::vector<std::string>{"inner"});
}

ZEST_CASE(CommasInCalls) {
    ZEXPECT(designators("f({ .a = g(1, .5), .") == std::vector<std::string>{"a"});
}

ZEST_CASE(DesignatedInPart) {
    llvm::StringRef text = "Shape s = { .origin.x = 1, .items[0] = 2, .id{3}, .color = 4, .";
    ZEXPECT(designators(text) == std::vector<std::string>{"origin", "items", "id", "color"});
    ZEXPECT(whole(text) == std::vector<std::string>{"id", "color"});
}

ZEST_CASE(ContinuedDesignator) {
    ZEXPECT(designators("Outer o = { .inner = 1, .other.").empty());
}

ZEST_CASE(MemberAccess) {
    ZEXPECT(designators("int v = { point.").empty());
}

ZEST_CASE(DirectiveBraces) {
    ZEXPECT(designators("#define OPEN {\nSize s = { .width = 1, .") ==
            std::vector<std::string>{"width"});
}

ZEST_CASE(DigitSeparator) {
    ZEXPECT(designators("Size s = { .width = 1'000, .") == std::vector<std::string>{"width"});
}

ZEST_CASE(TextAfterCursor) {
    ZEXPECT(designators("Size s = { .width = 1, .height = 2 };", 24) ==
            std::vector<std::string>{"width"});
}

};  // ZEST_SUITE(WrittenDesignators)

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

ZEST_CASE(InsideBlockComment) {
    llvm::StringRef text = "/*\n#include <vec\n*/\n";
    auto offset = static_cast<std::uint32_t>(text.find("vec") + 3);
    ZEXPECT(detect_completion_context(text, offset).kind == CompletionContext::None);
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
    ZEXPECT(ctx.closed);
    ZEXPECT(detect_completion_context("import std;\n", 11).kind == CompletionContext::None);
}

ZEST_CASE(ImportClosedPastComments) {
    ZEXPECT(detect_completion_context("import st /* x */ [[a]];", 9).closed);
    ZEXPECT(!detect_completion_context("import st // a; b\nint x;", 9).closed);
    ZEXPECT(!detect_completion_context("import st\nint x;", 9).closed);
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
    ZEXPECT(closed.closed);
    auto open = detect_completion_context("#include \"my header.h", 12);
    ZEXPECT(open.replace.end == 12u);
    ZEXPECT(!open.closed);
}

ZEST_CASE(ImportReplaceSpan) {
    auto ctx = detect_completion_context("import std.io", 10);
    ZEXPECT(ctx.replace.begin == 7u);
    ZEXPECT(ctx.replace.end == 13u);
}

ZEST_CASE(ImportMemberAccess) {
    ZEXPECT(detect_completion_context("import->x", 8).kind == CompletionContext::None);
}

ZEST_CASE(HasIncludeAngled) {
    auto ctx = detect_completion_context("#if __has_include(<sys/ty>)", 25);
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "sys/ty");
    ZEXPECT(ctx.replace.begin == 23u);
    ZEXPECT(ctx.replace.end == 25u);
}

ZEST_CASE(HasIncludeQuoted) {
    std::string text = "#elif defined(A) && __has_include_next(\"conf";
    auto ctx = detect_completion_context(text, text.size());
    ZEXPECT(ctx.kind == CompletionContext::IncludeQuoted);
    ZEXPECT(ctx.prefix == "conf");
}

ZEST_CASE(HasIncludeContinued) {
    std::string text = R"(#if defined(X) && \
    __has_include(<vec)";
    auto ctx = detect_completion_context(text, text.size());
    ZEXPECT(ctx.kind == CompletionContext::IncludeAngled);
    ZEXPECT(ctx.prefix == "vec");
}

ZEST_CASE(HasIncludeUnclosedName) {
    // An editor closes the parenthesis but not the name: it ends at `)`.
    std::string text = "#if __has_include(<ve) && __has_include(<b>)";
    auto ctx = detect_completion_context(text, 21);
    ZEXPECT(ctx.prefix == "ve");
    ZEXPECT(ctx.replace.end == 21u);
    ZEXPECT(!ctx.closed);
}

ZEST_CASE(HasIncludeWritten) {
    // Past a whole header name the cursor is in the condition again.
    std::string text = "#if __has_include(<a.h>) && ";
    ZEXPECT(detect_completion_context(text, text.size()).kind == CompletionContext::None);
    text += "__has_include(<b";
    ZEXPECT(detect_completion_context(text, text.size()).prefix == "b");
    ZEXPECT(detect_completion_context("#define H __has_include(<a", 26).kind ==
            CompletionContext::None);
}

ZEST_CASE(ModuleDeclaration) {
    auto ctx = detect_completion_context("module ma;\n", 9);
    ZEXPECT(ctx.kind == CompletionContext::ModuleDeclaration);
    ZEXPECT(ctx.prefix == "ma");
    ZEXPECT(ctx.closed);
    ZEXPECT(detect_completion_context("export module ma", 16).kind == CompletionContext::None);
    ZEXPECT(detect_completion_context("module.field", 12).kind == CompletionContext::None);
}

};  // ZEST_SUITE(DetectCompletionContext)

ZEST_SUITE(CompleteModuleImport) {

ZEST_CASE(PrefixMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.net", Fid{3});
    modules.add_module("my_lib", Fid{4});

    auto results = complete_module_import(modules, "std", {});
    ZEXPECT(results.size() == 3u);
    for(auto& name: results) {
        ZEXPECT(name.starts_with("std"));
    }
}

ZEST_CASE(EmptyPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = complete_module_import(modules, "", {});
    ZEXPECT(results.size() == 2u);
}

ZEST_CASE(NoMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = complete_module_import(modules, "xyz", {});
    ZEXPECT(results.empty());
}

ZEST_CASE(EmptyModules) {
    clice::DependencyGraph modules;
    auto results = complete_module_import(modules, "std", {});
    ZEXPECT(results.empty());
}

ZEST_CASE(DottedPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.core", Fid{3});
    modules.add_module("boost.asio", Fid{4});

    auto results = complete_module_import(modules, "std.", {});
    ZEXPECT(results.size() == 2u);
    for(auto& name: results) {
        ZEXPECT(name.starts_with("std."));
    }
}

ZEST_CASE(PrefixIsFullName) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});

    auto results = complete_module_import(modules, "std", {});
    ZEXPECT(results.size() == 2u);
}

ZEST_CASE(OwnPartitionsOnly) {
    clice::DependencyGraph modules;
    modules.add_module("foo", Fid{1});
    modules.add_module("foo:core", Fid{2});
    modules.add_module("foo:utils", Fid{3});
    modules.add_module("bar", Fid{4});
    modules.add_module("bar:impl", Fid{5});

    auto names = complete_module_import(modules, "", {.module_name = "foo"});
    ZEXPECT(names == std::vector<std::string>{":core", ":utils", "bar"});
    auto partitions = complete_module_import(modules, ":", {.module_name = "foo:utils"});
    ZEXPECT(partitions == std::vector<std::string>{":core"});
    ZEXPECT(complete_module_import(modules, ":", {}).empty());
    ZEXPECT(complete_module_import(modules, "foo:", {.module_name = "foo"}).empty());
}

ZEST_CASE(DottedModuleNames) {
    clice::DependencyGraph modules;
    modules.add_module("a", Fid{1});
    modules.add_module("a.b", Fid{2});
    modules.add_module("a.b:p", Fid{3});
    modules.add_module("a.b:q", Fid{4});
    modules.add_module("a:r", Fid{5});

    auto names = complete_module_import(modules, "", {.module_name = "a.b:q"});
    ZEXPECT(names == std::vector<std::string>{":p", "a"});
}

ZEST_CASE(InterfaceSkipsInternal) {
    clice::DependencyGraph modules;
    modules.add_module("m", Fid{1});
    modules.add_module("m:api", Fid{2});
    modules.add_module("m:detail", Fid{3}, true);

    std::vector<std::string> interface_only{":api"};
    std::vector<std::string> both{":api", ":detail"};
    ZEXPECT(complete_module_import(modules, ":", {.module_name = "m", .is_interface_unit = true}) ==
            interface_only);
    ZEXPECT(complete_module_import(modules, ":", {.module_name = "m"}) == both);
    ZEXPECT(complete_module_import(modules, ":", {.module_name = "m:impl"}) == both);
    ZEXPECT(
        complete_module_import(modules, ":", {.module_name = "m:api", .is_interface_unit = true})
            .empty());
}

};  // ZEST_SUITE(CompleteModuleImport)

ZEST_SUITE(CompleteModuleDeclaration) {

ZEST_CASE(PrimaryInterfacesOnly) {
    clice::DependencyGraph modules;
    modules.add_module("math", Fid{1});
    modules.add_module("math:detail", Fid{2});
    modules.add_module("matrix", Fid{3});
    modules.add_module("io", Fid{4});

    ZEXPECT(complete_module_declaration(modules, "ma") ==
            std::vector<std::string>{"math", "matrix"});
}

};  // ZEST_SUITE(CompleteModuleDeclaration)

}  // namespace
}  // namespace clice::testing
