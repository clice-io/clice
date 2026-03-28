#include "module_scan_fixture.h"
#include "test/test.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

// =============================================================================
// scan() — module declaration extraction (lexer-based, cppref coverage)
// =============================================================================

TEST_SUITE(ModuleScan) {

// Primary module interface: export module M;
TEST_CASE(PrimaryModuleInterface) {
    auto result = scan("export module mylib;");
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    EXPECT_FALSE(result.need_preprocess);
}

// Module implementation unit: module M;
TEST_CASE(ModuleImplementationUnit) {
    auto result = scan("module mylib;");
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_FALSE(result.is_interface_unit);
    EXPECT_FALSE(result.need_preprocess);
}

// Dotted module name: export module std.io;
TEST_CASE(DottedModuleName) {
    auto result = scan("export module std.io;");
    EXPECT_EQ(result.module_name, "std.io");
    EXPECT_TRUE(result.is_interface_unit);
}

// Deeply dotted module name: export module a.b.c.d;
TEST_CASE(DeeplyDottedModuleName) {
    auto result = scan("export module a.b.c.d;");
    EXPECT_EQ(result.module_name, "a.b.c.d");
    EXPECT_TRUE(result.is_interface_unit);
}

// Module partition interface: export module M:P;
TEST_CASE(PartitionInterface) {
    auto result = scan("export module mylib:core;");
    EXPECT_EQ(result.module_name, "mylib:core");
    EXPECT_TRUE(result.is_interface_unit);
}

// Module partition implementation: module M:P;
TEST_CASE(PartitionImplementation) {
    auto result = scan("module mylib:core;");
    EXPECT_EQ(result.module_name, "mylib:core");
    EXPECT_FALSE(result.is_interface_unit);
}

// Dotted module name + partition: export module a.b:p;
TEST_CASE(DottedModuleWithPartition) {
    auto result = scan("export module a.b:p;");
    EXPECT_EQ(result.module_name, "a.b:p");
    EXPECT_TRUE(result.is_interface_unit);
}

// Global module fragment with includes before module declaration.
TEST_CASE(GlobalModuleFragmentWithIncludes) {
    auto result = scan(R"(
module;
#include <stdlib.h>
#include "config.h"
export module mylib;
)");
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    ASSERT_EQ(result.includes.size(), 2u);
    EXPECT_EQ(result.includes[0].path, "stdlib.h");
    EXPECT_TRUE(result.includes[0].is_angled);
    EXPECT_EQ(result.includes[1].path, "config.h");
    EXPECT_FALSE(result.includes[1].is_angled);
}

// Conditional module declaration with #ifdef.
TEST_CASE(ConditionalModuleIfdef) {
    auto result = scan(R"(
#ifdef USE_MODULES
export module mylib;
#endif
)");
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_TRUE(result.need_preprocess);
}

// Conditional module declaration with #if __cpp_modules.
TEST_CASE(ConditionalModuleCppModules) {
    auto result = scan(R"(
#if __cpp_modules >= 201907L
export module mylib;
#endif
)");
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_TRUE(result.need_preprocess);
}

// Conditional module declaration in global module fragment.
TEST_CASE(ConditionalModuleInGMF) {
    auto result = scan(R"(
module;
#include <stdlib.h>
#ifdef USE_MODULES
export module mylib;
#endif
)");
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_TRUE(result.need_preprocess);
    ASSERT_EQ(result.includes.size(), 1u);
    EXPECT_EQ(result.includes[0].path, "stdlib.h");
}

// Module declaration NOT inside conditional (after a closed conditional block).
TEST_CASE(ModuleAfterClosedConditional) {
    auto result = scan(R"(
module;
#ifdef FOO
#include <optional.h>
#endif
export module mylib;
)");
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    EXPECT_FALSE(result.need_preprocess);
}

// Private module fragment marker should not override the real module declaration.
TEST_CASE(PrivateModuleFragment) {
    auto result = scan(R"(
export module mylib;
export int f();
module : private;
int f() { return 42; }
)");
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

};  // TEST_SUITE(ModuleScan)

// =============================================================================
// scan_module_decl() — lightweight preprocessor fallback
// =============================================================================

TEST_SUITE(ModuleDeclFallback) {

TEST_CASE(Basic) {
    ModuleScanFixture f("main.cppm", "export module mylib;");
    auto result = f.decl();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ConditionalWithDefine) {
    // Without -DUSE_MODULES: no module declaration.
    ModuleScanFixture f1("main.cppm", R"(
#ifdef USE_MODULES
export module mylib;
#endif
)");
    EXPECT_TRUE(f1.decl().module_name.empty());

    // With -DUSE_MODULES: module declaration found.
    ModuleScanFixture f2("main.cppm",
                         R"(
#ifdef USE_MODULES
export module mylib;
#endif
)",
                         {"-DUSE_MODULES"});
    auto result = f2.decl();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ConditionalIfExpr) {
    // Without the define: no module.
    ModuleScanFixture f1("main.cppm", R"(
#if ENABLE_MODULES >= 1
export module mylib;
#endif
)");
    EXPECT_TRUE(f1.decl().module_name.empty());

    // With the define: module found.
    ModuleScanFixture f2("main.cppm",
                         R"(
#if ENABLE_MODULES >= 1
export module mylib;
#endif
)",
                         {"-DENABLE_MODULES=1"});
    auto result = f2.decl();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(GMFWithConditional) {
    ModuleScanFixture f("main.cppm", R"(
module;
#include "config.h"
#ifdef USE_MODULES
export module mylib;
#endif
)");
    f.add_file("config.h", "#define USE_MODULES 1\n");
    auto result = f.decl();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ImplementationUnit) {
    ModuleScanFixture f("main.cpp", "module mylib;");
    auto result = f.decl();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_FALSE(result.is_interface_unit);
}

TEST_CASE(DottedName) {
    ModuleScanFixture f("main.cppm", "export module std.io;");
    auto result = f.decl();
    EXPECT_EQ(result.module_name, "std.io");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(Partition) {
    ModuleScanFixture f("main.cppm", "export module mylib:core;");
    auto result = f.decl();
    EXPECT_EQ(result.module_name, "mylib:core");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(NoModule) {
    ModuleScanFixture f("main.cpp", "int main() { return 0; }");
    auto result = f.decl();
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.is_interface_unit);
    EXPECT_TRUE(result.modules.empty());
}

};  // TEST_SUITE(ModuleDeclFallback)

}  // namespace
}  // namespace clice::testing
