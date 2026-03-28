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

// === scan() module declaration tests (cppref coverage) ===

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

// Dotted module name + partition: export module a.b:p.q;
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

// Conditional module declaration with #ifdef (common pattern: header/module dual-use).
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
    // The include before the conditional should still be captured.
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

// Private module fragment: module : private;
// This is not a module declaration — it's a fragment marker. scan() should
// still extract the actual module declaration before it.
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

// === scan_module_decl() tests ===

TEST_CASE(ModuleDeclBasic) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", "export module mylib;");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ModuleDeclConditionalWithDefine) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
#ifdef USE_MODULES
export module mylib;
#endif
)");

    // Without -DUSE_MODULES: no module declaration.
    auto args1 = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result1 = scan_module_decl(args1, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_TRUE(result1.module_name.empty());

    // With -DUSE_MODULES: module declaration found.
    auto args2 =
        std::vector<const char*>{"clang++", "-std=c++20", "-DUSE_MODULES", main_path.c_str()};
    auto result2 = scan_module_decl(args2, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_EQ(result2.module_name, "mylib");
    EXPECT_TRUE(result2.is_interface_unit);
}

TEST_CASE(ModuleDeclConditionalIfExpr) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
#if ENABLE_MODULES >= 1
export module mylib;
#endif
)");

    // Without the define: no module.
    auto args1 = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result1 = scan_module_decl(args1, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_TRUE(result1.module_name.empty());

    // With the define: module found.
    auto args2 = std::vector<const char*>{"clang++", "-std=c++20", "-DENABLE_MODULES=1",
                                           main_path.c_str()};
    auto result2 = scan_module_decl(args2, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_EQ(result2.module_name, "mylib");
    EXPECT_TRUE(result2.is_interface_unit);
}

TEST_CASE(ModuleDeclGMFWithConditional) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
module;
#include "config.h"
#ifdef USE_MODULES
export module mylib;
#endif
)");
    vfs->add("config.h", "#define USE_MODULES 1\n");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ModuleDeclImplementationUnit) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp", "module mylib;");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_FALSE(result.is_interface_unit);
}

TEST_CASE(ModuleDeclDottedName) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", "export module std.io;");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_EQ(result.module_name, "std.io");
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ModuleDeclPartition) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", "export module mylib:core;");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    // clang represents partition as "mylib:core".
    EXPECT_TRUE(result.module_name.find("mylib") != std::string::npos);
    EXPECT_TRUE(result.is_interface_unit);
}

TEST_CASE(ModuleDeclNoModule) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp", "int main() { return 0; }");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.is_interface_unit);
}

// === scan_precise() module import tests ===

TEST_CASE(PreciseModuleImport) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
export module mylib;
import other;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

TEST_CASE(PreciseMultipleImports) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
export module mylib;
import alpha;
import beta;
import gamma;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 3u);
    EXPECT_EQ(result.modules[0], "alpha");
    EXPECT_EQ(result.modules[1], "beta");
    EXPECT_EQ(result.modules[2], "gamma");
}

TEST_CASE(PreciseDottedModuleImport) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
export module mylib;
import std.io;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "std.io");
}

TEST_CASE(PrecisePartitionImport) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
export module mylib;
import :core;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    // Partition import should appear in modules list.
    ASSERT_GE(result.modules.size(), 1u);
}

TEST_CASE(PreciseExportImport) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
export module mylib;
export import other;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    // export import should still appear as a module dependency.
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

TEST_CASE(PreciseExportImportPartition) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
export module mylib;
export import :core;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    // export import of partition should appear.
    ASSERT_GE(result.modules.size(), 1u);
}

TEST_CASE(PreciseImplementationImport) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("impl.cpp");
    vfs->add("impl.cpp", R"(
module mylib;
import other;
)");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_FALSE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

TEST_CASE(PreciseGMFWithImport) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
module;
#include "config.h"
export module mylib;
import dep;
)");
    vfs->add("config.h", "// config\n");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_EQ(result.module_name, "mylib");
    EXPECT_TRUE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "dep");
}

TEST_CASE(PreciseModuleWithMixedIncludesAndImports) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cppm");
    vfs->add("main.cppm", R"(
module;
#include "legacy.h"
export module mylib;
import dep_a;
import dep_b;
export int f();
)");
    vfs->add("legacy.h", "int legacy_func();\n");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_EQ(result.module_name, "mylib");
    // GMF #include should appear in includes.
    ASSERT_GE(result.includes.size(), 1u);
    // Module imports should appear in modules.
    ASSERT_EQ(result.modules.size(), 2u);
    EXPECT_EQ(result.modules[0], "dep_a");
    EXPECT_EQ(result.modules[1], "dep_b");
}

TEST_CASE(PreciseNoModule) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    auto main_path = TestVFS::path("main.cpp");
    vfs->add("main.cpp", R"(
#include "header.h"
int main() { return 0; }
)");
    vfs->add("header.h", "int x;\n");

    auto args = std::vector<const char*>{"clang++", "-std=c++20", main_path.c_str()};
    auto result = scan_precise(args, TestVFS::root(), {}, nullptr, vfs);

    EXPECT_TRUE(result.module_name.empty());
    EXPECT_FALSE(result.is_interface_unit);
    EXPECT_TRUE(result.modules.empty());
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

}  // namespace
}  // namespace clice::testing
