#include "module_scan_fixture.h"
#include "test/test.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

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

// Partition import: clang returns the fully-qualified name "mylib:core"
// (owning module + ':' + partition name) as a single ModuleIdPath entry.
TEST_CASE(PartitionImport) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
import :core;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "mylib:core");
}

// Export-import of a named module.
TEST_CASE(ExportImport) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
export import other;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

// Export-import of a partition.
TEST_CASE(ExportImportPartition) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
export import :core;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "mylib:core");
}

// Implementation unit importing a named module.
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

// Implementation unit importing a partition of the same module.
TEST_CASE(ImplementationPartitionImport) {
    ModuleScanFixture f("impl.cpp", R"(
module mylib;
import :utils;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "mylib:utils");
}

// Multiple partition imports.
TEST_CASE(MultiplePartitionImports) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
export import :core;
import :utils;
import :io;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 3u);
    EXPECT_EQ(result.modules[0], "mylib:core");
    EXPECT_EQ(result.modules[1], "mylib:utils");
    EXPECT_EQ(result.modules[2], "mylib:io");
}

// Mixed named module imports and partition imports.
TEST_CASE(MixedNamedAndPartitionImports) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
import other;
export import :core;
import another.lib;
import :utils;
)");
    auto result = f.precise();
    ASSERT_EQ(result.modules.size(), 4u);
    EXPECT_EQ(result.modules[0], "other");
    EXPECT_EQ(result.modules[1], "mylib:core");
    EXPECT_EQ(result.modules[2], "another.lib");
    EXPECT_EQ(result.modules[3], "mylib:utils");
}

// NOTE: Header unit imports (import <header>; / import "header";) are not
// tested here because they require actual header unit compilation support
// which clang's PreprocessOnlyAction doesn't provide in a VFS-only context.
// These would hang trying to resolve system headers.

// GMF with imports.
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

// Mixed includes (from GMF) and imports (after module decl).
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

// No module — plain C++ file.
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

// Partition interface unit declaring and importing another partition.
TEST_CASE(PartitionInterfaceImportingPartition) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib:ui;
import :core;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib:ui");
    EXPECT_TRUE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "mylib:core");
}

// Partition implementation importing another partition.
TEST_CASE(PartitionImplImportingPartition) {
    ModuleScanFixture f("impl.cpp", R"(
module mylib:detail;
import :core;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib:detail");
    EXPECT_FALSE(result.is_interface_unit);
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "mylib:core");
}

// Import target is a macro-expanded name.
// C++20 forbids object-like macros in module DECLARATIONS (export module M;),
// but clang's preprocessor expands macros in import declarations.
TEST_CASE(ImportMacroExpandedName) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
#define OTHER_MOD other
import OTHER_MOD;
)");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "other");
}

// Import target from a macro defined on the command line.
TEST_CASE(ImportMacroFromCommandLine) {
    ModuleScanFixture f("main.cppm",
                        R"(
export module mylib;
import DEP_MOD;
)",
                        {"-DDEP_MOD=dependency"});
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "dependency");
}

// Import target from a macro defined in GMF header.
TEST_CASE(ImportMacroFromGMFHeader) {
    ModuleScanFixture f("main.cppm", R"(
module;
#include "deps.h"
export module mylib;
import MY_DEP;
)");
    f.add_file("deps.h", "#define MY_DEP some_lib\n");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "some_lib");
}

// Import target from a macro defined in a header #included AFTER the module
// declaration (not in GMF). C++20 allows #include after module declarations —
// the preprocessor still processes them and any macros they define are visible
// to subsequent import declarations.
TEST_CASE(ImportMacroFromPostDeclInclude) {
    ModuleScanFixture f("main.cppm", R"(
export module mylib;
#include "imports.h"
import MY_IMPORT;
)");
    f.add_file("imports.h", "#define MY_IMPORT dep\n");
    auto result = f.precise();
    EXPECT_EQ(result.module_name, "mylib");
    ASSERT_EQ(result.modules.size(), 1u);
    EXPECT_EQ(result.modules[0], "dep");
}

};  // TEST_SUITE(ModuleImportScan)

}  // namespace
}  // namespace clice::testing
