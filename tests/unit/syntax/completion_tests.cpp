#include "test/test.h"
#include "syntax/completion.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/DenseMap.h"

namespace clice::testing {
namespace {

TEST_SUITE(DetectCompletionContext) {

TEST_CASE(IncludeAngled) {
    auto ctx = detect_completion_context("#include <vec", 13);
    EXPECT_EQ(ctx.kind, CompletionContext::IncludeAngled);
    EXPECT_EQ(ctx.prefix, "vec");
}

TEST_CASE(IncludeQuoted) {
    auto ctx = detect_completion_context("#include \"my_header", 19);
    EXPECT_EQ(ctx.kind, CompletionContext::IncludeQuoted);
    EXPECT_EQ(ctx.prefix, "my_header");
}

TEST_CASE(IncludeAngledWithSpaces) {
    auto ctx = detect_completion_context("  #  include  <sys/", 19);
    EXPECT_EQ(ctx.kind, CompletionContext::IncludeAngled);
    EXPECT_EQ(ctx.prefix, "sys/");
}

TEST_CASE(IncludeEmpty) {
    auto ctx = detect_completion_context("#include <", 10);
    EXPECT_EQ(ctx.kind, CompletionContext::IncludeAngled);
    EXPECT_EQ(ctx.prefix, "");
}

TEST_CASE(CursorInsideKeyword) {
    // A keyword is only "typed" once the cursor passed its end.
    EXPECT_EQ(detect_completion_context("#include", 5).kind, CompletionContext::None);
    EXPECT_EQ(detect_completion_context("import", 3).kind, CompletionContext::None);
}

TEST_CASE(CursorAtNewline) {
    // A cursor sitting on a line's terminating newline still completes the
    // statement that line opened.
    auto ctx = detect_completion_context("import std\nint x;", 10);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "std");
}

TEST_CASE(CrlfLineEndings) {
    auto ctx = detect_completion_context("#include <a>\r\n#include <ve", 26);
    EXPECT_EQ(ctx.kind, CompletionContext::IncludeAngled);
    EXPECT_EQ(ctx.prefix, "ve");
}

TEST_CASE(ImportSimple) {
    auto ctx = detect_completion_context("import std", 10);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "std");
}

TEST_CASE(ExportImport) {
    auto ctx = detect_completion_context("export import my_mod", 20);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "my_mod");
}

TEST_CASE(ImportWithSemicolon) {
    auto ctx = detect_completion_context("import std;\n", 7);
    EXPECT_EQ(ctx.kind, CompletionContext::None);
}

TEST_CASE(ImportEmpty) {
    auto ctx = detect_completion_context("import ", 7);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "");
}

TEST_CASE(NormalCode) {
    auto ctx = detect_completion_context("int main() {", 12);
    EXPECT_EQ(ctx.kind, CompletionContext::None);
}

TEST_CASE(MultilineAtSecondLine) {
    std::string text = "#include <vector>\n#include <str";
    auto ctx = detect_completion_context(text, text.size());
    EXPECT_EQ(ctx.kind, CompletionContext::IncludeAngled);
    EXPECT_EQ(ctx.prefix, "str");
}

TEST_CASE(NotImportKeyword) {
    auto ctx = detect_completion_context("importlib foo", 13);
    EXPECT_EQ(ctx.kind, CompletionContext::None);
}

TEST_CASE(HashOnly) {
    auto ctx = detect_completion_context("#", 1);
    EXPECT_EQ(ctx.kind, CompletionContext::None);
}

TEST_CASE(ImportDottedPrefix) {
    auto ctx = detect_completion_context("import std.io", 13);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "std.io");
}

TEST_CASE(ImportPartitionPrefix) {
    auto ctx = detect_completion_context("import :core", 12);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, ":core");
}

TEST_CASE(ImportPartitionEmpty) {
    auto ctx = detect_completion_context("import :", 8);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, ":");
}

TEST_CASE(ImportWithLeadingSpaces) {
    auto ctx = detect_completion_context("  import std", 12);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "std");
}

TEST_CASE(ExportImportEmpty) {
    auto ctx = detect_completion_context("export import ", 14);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "");
}

TEST_CASE(ImportAfterNewline) {
    std::string text = "module foo;\nimport ";
    auto ctx = detect_completion_context(text, text.size());
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "");
}

TEST_CASE(ImportCursorMidLine) {
    // The prefix is truncated at the cursor; trailing text is ignored.
    auto ctx = detect_completion_context("import std.io", 10);
    EXPECT_EQ(ctx.kind, CompletionContext::Import);
    EXPECT_EQ(ctx.prefix, "std");
}

};  // TEST_SUITE(DetectCompletionContext)

TEST_SUITE(CompleteModuleImport) {

TEST_CASE(PrefixMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.net", Fid{3});
    modules.add_module("my_lib", Fid{4});

    auto results = complete_module_import(modules, "std");
    EXPECT_EQ(results.size(), 3u);
    for(auto& name: results) {
        EXPECT_TRUE(name.starts_with("std"));
    }
}

TEST_CASE(EmptyPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = complete_module_import(modules, "");
    EXPECT_EQ(results.size(), 2u);
}

TEST_CASE(NoMatch) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("my_lib", Fid{2});

    auto results = complete_module_import(modules, "xyz");
    EXPECT_TRUE(results.empty());
}

TEST_CASE(EmptyModules) {
    clice::DependencyGraph modules;
    auto results = complete_module_import(modules, "std");
    EXPECT_TRUE(results.empty());
}

TEST_CASE(DottedPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});
    modules.add_module("std.core", Fid{3});
    modules.add_module("boost.asio", Fid{4});

    auto results = complete_module_import(modules, "std.");
    EXPECT_EQ(results.size(), 2u);
    for(auto& name: results) {
        EXPECT_TRUE(name.starts_with("std."));
    }
}

TEST_CASE(PartitionPrefix) {
    clice::DependencyGraph modules;
    modules.add_module("foo", Fid{1});
    modules.add_module("foo:core", Fid{2});
    modules.add_module("foo:utils", Fid{3});
    modules.add_module("bar:impl", Fid{4});

    auto results = complete_module_import(modules, "foo:");
    EXPECT_EQ(results.size(), 2u);
    for(auto& name: results) {
        EXPECT_TRUE(name.starts_with("foo:"));
    }
}

TEST_CASE(PrefixIsFullName) {
    clice::DependencyGraph modules;
    modules.add_module("std", Fid{1});
    modules.add_module("std.io", Fid{2});

    auto results = complete_module_import(modules, "std");
    EXPECT_EQ(results.size(), 2u);
}

};  // TEST_SUITE(CompleteModuleImport)

}  // namespace
}  // namespace clice::testing
