#include "test/test.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

/// Helper that sets up a TestVFS with a main file and optional extra files,
/// then calls the given scan function with standard C++20 arguments.
struct ModuleScanFixture {
    llvm::IntrusiveRefCntPtr<TestVFS> vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    std::string main_path;
    std::vector<const char*> args;

    /// Create fixture with main file content and optional extra defines.
    ModuleScanFixture(llvm::StringRef filename,
                      llvm::StringRef content,
                      std::initializer_list<const char*> extra_args = {}) {
        main_path = TestVFS::path(filename);
        vfs->add(filename, content);
        args.push_back("clang++");
        args.push_back("-std=c++20");
        for(auto a: extra_args) {
            args.push_back(a);
        }
        args.push_back(main_path.c_str());
    }

    void add_file(llvm::StringRef name, llvm::StringRef content = "") {
        vfs->add(name, content);
    }

    ScanResult decl() {
        return scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    }

    ScanResult precise() {
        return scan_precise(args, TestVFS::root(), {}, nullptr, vfs);
    }
};

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
    EXPECT_TRUE(result.module_name.find("mylib") != std::string::npos);
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(NoModule) {
    ModuleScanFixture f("main.cpp", "int main() { return 0; }");
    auto result = f.decl();
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.is_interface_unit);
}

};  // TEST_SUITE(ModuleDeclFallback)

// =============================================================================
// scan_precise() — module import semantics
// =============================================================================

TEST_SUITE(ModuleImportScan) {

TEST_CASE(NamedImport) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
import other;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

TEST_CASE(MultipleImports) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
import alpha;
import beta;
import gamma;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 3u);
    EXPECT_EQ(result.modules[0], "alpha");
    EXPECT_EQ(result.modules[1], "beta");
    EXPECT_EQ(result.modules[2], "gamma");
}

TEST_CASE(DottedModuleImport) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
import std.io;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "std.io");
}

TEST_CASE(PartitionImport) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
import :core;
)");
    auto result = f.precise();
    ASSERT_GE(result.modules.size(), 1u);
}

TEST_CASE(ExportImport) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
export import other;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

TEST_CASE(ExportImportPartition) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
export import :core;
)");
    auto result = f.precise();
    ASSERT_GE(result.modules.size(), 1u);
}

TEST_CASE(ImplementationImport) {
    ModuleScanFixture f("impl.cpp", R"(
module mylib;
import other;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_FALSE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

TEST_CASE(GMFWithImport) {
    ModuleScanFixture f("main.cppm", R"(
module;
#include "config.h"
export module mylib;
import dep;
)");
    f.add_file("config.h", "// config\n");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "dep");
}

TEST_CASE(MixedIncludesAndImports) {
    ModuleScanFixture f("main.cppm", R"(
module;
#include "legacy.h"
export module mylib;
import dep_a;
import dep_b;
export int f();
)");
    f.add_file("legacy.h", "int legacy_func();\n");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_GE(result.includes.size(), 1u);
    ASSERT_EQ(result.modules.size(), 2u);
    EXPECT_EQ(result.modules[0], "dep_a");
    EXPECT_EQ(result.modules[1], "dep_b");
}

TEST_CASE(NoModule) {
    ModuleScanFixture f("main.cpp", R"(
#include "header.h"
int main() { return 0; }
)");
    f.add_file("header.h", "int x;\n");
    auto result = f.precise();
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.is_interface_unit);
    EXPECT_TRUE(result.modules.empty());
}

};  // TEST_SUITE(ModuleImportScan)

}  // namespace
}  // namespace clice::testing
