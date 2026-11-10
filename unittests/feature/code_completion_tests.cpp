module clice;

import :command.search_config;
import :command.table;
import :feature.code_completion.cursor;
import :feature.code_completion.include_paths;
import :feature.code_completion.module_names;
import :syntax.dependency_graph;
import :syntax.lexer;
import :syntax.scan;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.dir_cache;

namespace clice::testing {
namespace {

using feature::CompletionName;
using feature::IncludeCandidate;
using feature::module_providers;
using feature::WrittenDesignator;

bool follows_access_operator(llvm::StringRef text, std::uint32_t offset) {
    return feature::follows_access_operator(text, offset, dialect_of());
}

std::vector<WrittenDesignator> written_designators(llvm::StringRef text, std::uint32_t offset) {
    return feature::written_designators(text, offset, dialect_of());
}

CompletionName completion_name(llvm::StringRef text, std::uint32_t offset) {
    return feature::completion_name(text, offset, dialect_of());
}

/// The module names a completion offers, in their order.
std::vector<std::string> names_of(const std::vector<feature::ModuleCandidate>& candidates) {
    return ranges::to<std::vector>(candidates | views::transform(&feature::ModuleCandidate::name));
}

std::vector<std::string> import_names(const feature::ModuleProviders& providers,
                                      llvm::StringRef typed,
                                      const ScanResult& unit) {
    return names_of(feature::complete_module_import(providers, typed, unit));
}

std::vector<std::string> declaration_names(const feature::ModuleProviders& providers,
                                           llvm::StringRef typed) {
    return names_of(feature::complete_module_declaration(providers, typed));
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

ZEST_SUITE(CompletionName) {

ZEST_CASE(IncludeAngled) {
    auto ctx = completion_name("#include <vec", 13);
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "vec");
}

ZEST_CASE(IncludeQuoted) {
    auto ctx = completion_name("#include \"my_header", 19);
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeQuoted);
    ZEXPECT(ctx.prefix == "my_header");
}

ZEST_CASE(IncludeAngledWithSpaces) {
    auto ctx = completion_name("  #  include  <sys/", 19);
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "sys/");
}

ZEST_CASE(IncludeEmpty) {
    auto ctx = completion_name("#include <", 10);
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(CursorInsideKeyword) {
    // A keyword is only "typed" once the cursor passed its end; inside a
    // directive's name, the name is.
    auto directive = completion_name("#include", 5);
    ZEXPECT(directive.kind == CompletionName::Kind::Directive);
    ZEXPECT(directive.prefix == "incl");
    ZEXPECT(directive.replace == LocalSourceRange(1, 8));
    ZEXPECT(completion_name("import", 3).kind == CompletionName::Kind::None);
}

ZEST_CASE(InsideBlockComment) {
    llvm::StringRef text = "/*\n#include <vec\n*/\n";
    auto offset = static_cast<std::uint32_t>(text.find("vec") + 3);
    ZEXPECT(completion_name(text, offset).kind == CompletionName::Kind::None);
}

ZEST_CASE(CursorAtNewline) {
    // A cursor sitting on a line's terminating newline still completes the
    // statement that line opened.
    auto ctx = completion_name("import std\nint x;", 10);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(CrlfLineEndings) {
    auto ctx = completion_name("#include <a>\r\n#include <ve", 26);
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "ve");
}

ZEST_CASE(ImportSimple) {
    auto ctx = completion_name("import std", 10);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(ExportImport) {
    auto ctx = completion_name("export import my_mod", 20);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "my_mod");
}

ZEST_CASE(ImportWithSemicolon) {
    auto ctx = completion_name("import st;\n", 9);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "st");
    ZEXPECT(ctx.closed);
    ZEXPECT(completion_name("import std;\n", 11).kind == CompletionName::Kind::None);
}

ZEST_CASE(ImportClosedPastComments) {
    ZEXPECT(completion_name("import st /* x */ [[a]];", 9).closed);
    ZEXPECT(!completion_name("import st // a; b\nint x;", 9).closed);
    ZEXPECT(!completion_name("import st\nint x;", 9).closed);
}

ZEST_CASE(ImportEmpty) {
    auto ctx = completion_name("import ", 7);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(NormalCode) {
    auto ctx = completion_name("int main() {", 12);
    ZEXPECT(ctx.kind == CompletionName::Kind::None);
}

ZEST_CASE(MultilineAtSecondLine) {
    std::string text = "#include <vector>\n#include <str";
    auto ctx = completion_name(text, text.size());
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "str");
}

ZEST_CASE(NotImportKeyword) {
    auto ctx = completion_name("importlib foo", 13);
    ZEXPECT(ctx.kind == CompletionName::Kind::None);
}

ZEST_CASE(HashOnly) {
    auto ctx = completion_name("#", 1);
    ZEXPECT(ctx.kind == CompletionName::Kind::Directive);
    ZEXPECT(ctx.prefix.empty());
    ZEXPECT(ctx.replace == LocalSourceRange(1, 1));
}

ZEST_CASE(DirectiveInConditional) {
    using enum CompletionName::Conditional;
    ZEXPECT(completion_name("#ifndef G\n#el", 13).conditional == Open);
    ZEXPECT(completion_name("#if A\n#endif\n#el", 16).conditional == None);
    ZEXPECT(completion_name("#if A\n#else\n#e", 14).conditional == Else);
    ZEXPECT(completion_name("#if A\n#else\n#if B\n#e", 20).conditional == Open);
}

ZEST_CASE(DirectiveArgument) {
    auto ctx = completion_name("#ifdef FO", 9);
    ZEXPECT(ctx.kind == CompletionName::Kind::None);
    ZEXPECT(ctx.directive == "ifdef");
    ZEXPECT(completion_name("#include <ve", 12).directive == "include");
    ZEXPECT(completion_name("#include_next <ve", 17).kind == CompletionName::Kind::IncludeAngled);
}

ZEST_CASE(CommentAfterHash) {
    ZEXPECT(completion_name("# // see fo", 11).kind == CompletionName::Kind::None);
}

ZEST_CASE(DirectiveAfterSpaces) {
    auto ctx = completion_name("#  if", 5);
    ZEXPECT(ctx.kind == CompletionName::Kind::Directive);
    ZEXPECT(ctx.prefix == "if");
    ZEXPECT(completion_name("#if ", 4).kind == CompletionName::Kind::None);
}

ZEST_CASE(ImportDottedPrefix) {
    auto ctx = completion_name("import std.io", 13);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "std.io");
}

ZEST_CASE(ImportPartitionPrefix) {
    auto ctx = completion_name("import :core", 12);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == ":core");
}

ZEST_CASE(ImportPartitionEmpty) {
    auto ctx = completion_name("import :", 8);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == ":");
}

ZEST_CASE(ImportWithLeadingSpaces) {
    auto ctx = completion_name("  import std", 12);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(ExportImportEmpty) {
    auto ctx = completion_name("export import ", 14);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(ImportAfterNewline) {
    std::string text = "module foo;\nimport ";
    auto ctx = completion_name(text, text.size());
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "");
}

ZEST_CASE(ImportCursorMidLine) {
    // The prefix is truncated at the cursor; trailing text is ignored.
    auto ctx = completion_name("import std.io", 10);
    ZEXPECT(ctx.kind == CompletionName::Kind::Import);
    ZEXPECT(ctx.prefix == "std");
}

ZEST_CASE(IncludeReplaceSpan) {
    // The last path component through its untyped rest, delimiter excluded.
    auto ctx = completion_name("#include <sys/ty.h>", 16);
    ZEXPECT(ctx.prefix == "sys/ty");
    ZEXPECT(ctx.replace.begin == 14u);
    ZEXPECT(ctx.replace.end == 18u);
}

ZEST_CASE(IncludeNameWithSpaces) {
    auto closed = completion_name("#include \"my header.h\"", 12);
    ZEXPECT(closed.replace.end == 21u);
    ZEXPECT(closed.closed);
    auto open = completion_name("#include \"my header.h", 12);
    ZEXPECT(open.replace.end == 12u);
    ZEXPECT(!open.closed);
}

ZEST_CASE(ImportReplaceSpan) {
    auto ctx = completion_name("import std.io", 10);
    ZEXPECT(ctx.replace.begin == 7u);
    ZEXPECT(ctx.replace.end == 13u);
}

ZEST_CASE(ImportMemberAccess) {
    ZEXPECT(completion_name("import->x", 8).kind == CompletionName::Kind::None);
}

ZEST_CASE(HasIncludeAngled) {
    auto ctx = completion_name("#if __has_include(<sys/ty>)", 25);
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "sys/ty");
    ZEXPECT(ctx.replace.begin == 23u);
    ZEXPECT(ctx.replace.end == 25u);
}

ZEST_CASE(HasIncludeQuoted) {
    std::string text = "#elif defined(A) && __has_include_next(\"conf";
    auto ctx = completion_name(text, text.size());
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeQuoted);
    ZEXPECT(ctx.prefix == "conf");
}

ZEST_CASE(HasIncludeContinued) {
    std::string text = R"(#if defined(X) && \
    __has_include(<vec)";
    auto ctx = completion_name(text, text.size());
    ZEXPECT(ctx.kind == CompletionName::Kind::IncludeAngled);
    ZEXPECT(ctx.prefix == "vec");
}

ZEST_CASE(HasIncludeUnclosedName) {
    // An editor closes the parenthesis but not the name: it ends at `)`.
    std::string text = "#if __has_include(<ve) && __has_include(<b>)";
    auto ctx = completion_name(text, 21);
    ZEXPECT(ctx.prefix == "ve");
    ZEXPECT(ctx.replace.end == 21u);
    ZEXPECT(!ctx.closed);
}

ZEST_CASE(HasIncludeWritten) {
    // Past a whole header name the cursor is in the condition again.
    std::string text = "#if __has_include(<a.h>) && ";
    ZEXPECT(completion_name(text, text.size()).kind == CompletionName::Kind::None);
    text += "__has_include(<b";
    ZEXPECT(completion_name(text, text.size()).prefix == "b");
    ZEXPECT(completion_name("#define H __has_include(<a", 26).kind == CompletionName::Kind::None);
}

ZEST_CASE(ModuleDeclaration) {
    auto ctx = completion_name("module ma;\n", 9);
    ZEXPECT(ctx.kind == CompletionName::Kind::ModuleDeclaration);
    ZEXPECT(ctx.prefix == "ma");
    ZEXPECT(ctx.closed);
    ZEXPECT(completion_name("export module ma", 16).kind == CompletionName::Kind::None);
    ZEXPECT(completion_name("module.field", 12).kind == CompletionName::Kind::None);
}

};  // ZEST_SUITE(CompletionName)

ZEST_SUITE(CompleteModuleImport) {

ZEST_CASE(PrefixMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.net", Fid{3});
    modules.add_module("my_lib", Fid{4});

    auto results = import_names(module_providers(modules), "std", {});
    ZEXPECT(results.size() == 3u);
    for(auto& name: results) {
        ZEXPECT(name.starts_with("std"));
    }
}

ZEST_CASE(EmptyPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = import_names(module_providers(modules), "", {});
    ZEXPECT(results.size() == 2u);
}

ZEST_CASE(NoMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = import_names(module_providers(modules), "xyz", {});
    ZEXPECT(results.empty());
}

ZEST_CASE(EmptyModules) {
    clice::DependencyGraph modules;
    auto results = import_names(module_providers(modules), "std", {});
    ZEXPECT(results.empty());
}

ZEST_CASE(DottedPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.core", Fid{3});
    modules.add_module("boost.asio", Fid{4});

    auto results = import_names(module_providers(modules), "std.", {});
    ZEXPECT(results.size() == 2u);
    for(auto& name: results) {
        ZEXPECT(name.starts_with("std."));
    }
}

ZEST_CASE(PrefixIsFullName) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});

    auto results = import_names(module_providers(modules), "std", {});
    ZEXPECT(results.size() == 2u);
}

ZEST_CASE(WordsOfNames) {
    /// A module name matches by the words of its name, the better match
    /// first: the whole name before a word of a longer one.
    clice::DependencyGraph modules;
    modules.add_module("std.compat", Fid{1});
    modules.add_module("compat", Fid{2});
    modules.add_module("boost", Fid{3});

    auto names = import_names(module_providers(modules), "compat", {});
    ZEXPECT(names == std::vector<std::string>{"compat", "std.compat"});
}

ZEST_CASE(OwnPartitionsOnly) {
    clice::DependencyGraph modules;
    modules.add_module("foo", Fid{1});
    modules.add_module("foo:core", Fid{2});
    modules.add_module("foo:utils", Fid{3});
    modules.add_module("bar", Fid{4});
    modules.add_module("bar:impl", Fid{5});

    auto names = import_names(module_providers(modules), "", {.module_name = "foo"});
    ZEXPECT(names == std::vector<std::string>{":core", ":utils", "bar"});
    auto partitions = import_names(module_providers(modules), ":", {.module_name = "foo:utils"});
    ZEXPECT(partitions == std::vector<std::string>{":core"});
    ZEXPECT(import_names(module_providers(modules), ":", {}).empty());
    ZEXPECT(import_names(module_providers(modules), "foo:", {.module_name = "foo"}).empty());
}

ZEST_CASE(DottedModuleNames) {
    clice::DependencyGraph modules;
    modules.add_module("a", Fid{1});
    modules.add_module("a.b", Fid{2});
    modules.add_module("a.b:p", Fid{3});
    modules.add_module("a.b:q", Fid{4});
    modules.add_module("a:r", Fid{5});

    auto names = import_names(module_providers(modules), "", {.module_name = "a.b:q"});
    ZEXPECT(names == std::vector<std::string>{":p", "a"});
}

ZEST_CASE(InterfaceSkipsInternal) {
    clice::DependencyGraph modules;
    modules.add_module("m", Fid{1});
    modules.add_module("m:api", Fid{2});
    modules.add_module("m:detail", Fid{3}, true);

    std::vector<std::string> interface_only{":api"};
    std::vector<std::string> both{":api", ":detail"};
    ZEXPECT(import_names(module_providers(modules),
                         ":",
                         {.module_name = "m", .is_interface_unit = true}) == interface_only);
    ZEXPECT(import_names(module_providers(modules), ":", {.module_name = "m"}) == both);
    ZEXPECT(import_names(module_providers(modules), ":", {.module_name = "m:impl"}) == both);
    ZEXPECT(import_names(module_providers(modules),
                         ":",
                         {.module_name = "m:api", .is_interface_unit = true})
                .empty());
}

ZEST_CASE(OpenBuffersDeclare) {
    clice::DependencyGraph modules;
    modules.add_module("old_name", Fid{1});
    modules.add_module("kept", Fid{2});
    modules.add_module("m:detail", Fid{3}, true);

    // The buffer renames its module, a new unit declares one, an internal
    // partition becomes an interface one.
    auto providers = module_providers(modules,
                                      {
                                          {.file = Fid{1}, .name = "new_name"},
                                          {.file = Fid{4}, .name = "fresh"   },
                                          {.file = Fid{3}, .name = "m:detail"},
    });
    ZEXPECT(import_names(providers, "", {}) ==
            std::vector<std::string>{"fresh", "kept", "new_name"});
    ZEXPECT(import_names(providers, ":", {.module_name = "m", .is_interface_unit = true}) ==
            std::vector<std::string>{":detail"});
}

};  // ZEST_SUITE(CompleteModuleImport)

ZEST_SUITE(CompleteModuleDeclaration) {

ZEST_CASE(PrimaryInterfacesOnly) {
    clice::DependencyGraph modules;
    modules.add_module("math", Fid{1});
    modules.add_module("math:detail", Fid{2});
    modules.add_module("matrix", Fid{3});
    modules.add_module("io", Fid{4});

    ZEXPECT(declaration_names(module_providers(modules), "ma") ==
            std::vector<std::string>{"math", "matrix"});
}

};  // ZEST_SUITE(CompleteModuleDeclaration)

ZEST_SUITE(CompleteIncludePath) {

/// The names an include completes to, in their order.
std::vector<std::string> complete(const SearchConfig& config, llvm::StringRef prefix) {
    vfs::DirCache cache;
    vfs::Scope scope(cache);
    auto candidates = feature::complete_include_path(config, "", prefix, true, scope);
    ranges::sort(candidates, {}, [](const IncludeCandidate& candidate) {
        return std::tie(candidate.order, candidate.name);
    });
    std::vector<std::string> names;
    for(auto& candidate: candidates) {
        names.push_back(candidate.is_directory ? candidate.name + "/" : candidate.name);
    }
    return names;
}

ZEST_CASE(WordsOfNames) {
    /// The typed name matches the whole name first, then a prefix, then
    /// words of a name.
    TempDir tmp;
    tmp.touch("inc/view");
    tmp.touch("inc/viewport.h");
    tmp.touch("inc/string_view");
    tmp.touch("inc/vector");
    SearchConfig config;
    config.dirs.push_back({.path = tmp.path("inc"), .driver = true});
    config.system_start_idx = 0;
    ZEXPECT(complete(config, "view") ==
            std::vector<std::string>{"view", "viewport.h", "string_view"});
}

ZEST_CASE(OtherFilesLast) {
    /// Files no header is named as follow the headers, a hidden one all.
    TempDir tmp;
    tmp.touch("inc/notes.txt");
    tmp.touch("inc/.hidden.h");
    tmp.touch("inc/notes.h");
    SearchConfig config;
    config.dirs.push_back({tmp.path("inc")});
    ZEXPECT(complete(config, "notes") == std::vector<std::string>{"notes.h", "notes.txt"});
    ZEXPECT(complete(config, "") == std::vector<std::string>{"notes.h", "notes.txt", ".hidden.h"});
}

ZEST_CASE(HeadersOutrankOtherFiles) {
    /// A name an earlier directory holds as some other file is listed at
    /// the rank of the header a later one holds.
    TempDir tmp;
    tmp.touch("inc/print");
    tmp.touch("sys/print");
    tmp.touch("sys/printer.h");
    SearchConfig config;
    config.dirs.push_back({tmp.path("inc")});
    config.dirs.push_back({tmp.path("sys")});
    config.system_start_idx = 1;
    ZEXPECT(complete(config, "pri") == std::vector<std::string>{"print", "printer.h"});
}

ZEST_CASE(InternalBuiltinsLast) {
    /// clang's builtin headers that must not be included directly follow
    /// the others.
    SearchConfig config;
    config.dirs.push_back(
        {.path = Spelling::absolute(path::join(resource_dir(), "include")).str(), .driver = true});
    auto names = complete(config, "va");
    ZASSERT(names.size() >= 3u);
    ZEXPECT(names.back() == "vaesintrin.h");
    ZEXPECT(names[0] == "vadefs.h");
    ZEXPECT(names[1] == "varargs.h");
}

ZEST_CASE(Frameworks) {
    /// A framework directory completes to its frameworks' names, and past
    /// one to the headers of its Headers/.
    TempDir tmp;
    tmp.touch("Frameworks/Foo.framework/Headers/Bar.h");
    tmp.touch("Frameworks/Foo.framework/Headers/Baz");
    tmp.touch("Frameworks/Foo.framework/Resources/Info.plist");
    tmp.touch("Frameworks/stray.h");
    SearchConfig config;
    config.dirs.push_back({.path = tmp.path("Frameworks"), .framework = true});
    ZEXPECT(complete(config, "") == std::vector<std::string>{"Foo/"});
    ZEXPECT(complete(config, "Foo/") == std::vector<std::string>{"Bar.h", "Baz"});
}

};  // ZEST_SUITE(CompleteIncludePath)

}  // namespace
}  // namespace clice::testing
