#include "test/test.h"
#include "syntax/completion.h"

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

};  // TEST_SUITE(DetectCompletionContext)

TEST_SUITE(CompleteModuleImport) {

TEST_CASE(PrefixMatch) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    modules[1] = "std";
    modules[2] = "std.io";
    modules[3] = "std.net";
    modules[4] = "my_lib";

    auto results = complete_module_import(modules, "std");
    EXPECT_EQ(results.size(), 3u);
    for(auto& name: results) {
        EXPECT_TRUE(name.starts_with("std"));
    }
}

TEST_CASE(EmptyPrefix) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    modules[1] = "std";
    modules[2] = "my_lib";

    auto results = complete_module_import(modules, "");
    EXPECT_EQ(results.size(), 2u);
}

TEST_CASE(NoMatch) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    modules[1] = "std";
    modules[2] = "my_lib";

    auto results = complete_module_import(modules, "xyz");
    EXPECT_TRUE(results.empty());
}

TEST_CASE(EmptyModules) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    auto results = complete_module_import(modules, "std");
    EXPECT_TRUE(results.empty());
}

TEST_CASE(DottedPrefix) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    modules[1] = "std";
    modules[2] = "std.io";
    modules[3] = "std.core";
    modules[4] = "boost.asio";

    auto results = complete_module_import(modules, "std.");
    EXPECT_EQ(results.size(), 2u);
    for(auto& name: results) {
        EXPECT_TRUE(name.starts_with("std."));
    }
}

TEST_CASE(PartitionPrefix) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    modules[1] = "foo";
    modules[2] = "foo:core";
    modules[3] = "foo:utils";
    modules[4] = "bar:impl";

    auto results = complete_module_import(modules, "foo:");
    EXPECT_EQ(results.size(), 2u);
    for(auto& name: results) {
        EXPECT_TRUE(name.starts_with("foo:"));
    }
}

TEST_CASE(ExactMatch) {
    llvm::DenseMap<std::uint32_t, std::string> modules;
    modules[1] = "std";
    modules[2] = "std.io";

    auto results = complete_module_import(modules, "std");
    EXPECT_EQ(results.size(), 2u);
}

};  // TEST_SUITE(CompleteModuleImport)

}  // namespace
}  // namespace clice::testing
