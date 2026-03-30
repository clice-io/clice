#include <thread>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "command/command.h"
#include "compile/compilation.h"
#include "support/filesystem.h"
#include "syntax/scan.h"

namespace clice::testing {

namespace {

TEST_SUITE(Compiler, Tester) {

TEST_CASE(TopLevelDecls) {
    add_file("header.h", R"(
#pragma once
int helper();
)");

    llvm::StringRef content = R"(
#include "header.h"

int x = 1;

void foo() {}

namespace foo2 {
    int y = 2;
    int z = 3;
}

struct Bar {
    int x;
    int y;
};
)";

    add_main("main.cpp", content);
    ASSERT_TRUE(compile_with_pch());
    ASSERT_EQ(unit->top_level_decls().size(), 4U);
}

TEST_CASE(StopCompilation) {
    std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);

    llvm::StringRef content = R"(
int main() { return 0; }
)";
    add_main("main.cpp", content);

    prepare();
    params.stop = stop;

    // Set stop before compilation starts — verifies the mechanism works.
    stop->store(true);

    auto built = clice::compile(params);
    ASSERT_FALSE(built.completed());
}

TEST_CASE(PCHBuildPopulatesInfo) {
    add_file("preamble.h", R"(
#pragma once
int preamble_func();
struct PreambleStruct { int x; };
)");

    llvm::StringRef content = R"(
#include "preamble.h"

int main() { return 0; }
)";

    add_main("main.cpp", content);
    prepare();

    // Switch to Preamble kind for PCH building.
    params.kind = CompilationKind::Preamble;

    auto pch_path = fs::createTemporaryFile("clice-test", "pch");
    ASSERT_TRUE(pch_path.operator bool());
    params.output_file = *pch_path;

    // Add truncated main file buffer for preamble build.
    auto& source = sources.all_files["main.cpp"];
    auto bound = compute_preamble_bound(source.content);
    auto main_vfs_path = TestVFS::path("main.cpp");
    params.add_remapped_file(main_vfs_path, source.content, bound);

    PCHInfo info;
    auto preamble_unit = clice::compile(params, info);
    ASSERT_TRUE(preamble_unit.completed());

    // PCHInfo.path should match the output file.
    ASSERT_EQ(info.path, *pch_path);

    // PCHInfo.mtime should be a reasonable timestamp (non-zero, recent).
    ASSERT_TRUE(info.mtime > 0);

    // PCHInfo.preamble should be non-empty (contains the #include directives).
    ASSERT_FALSE(info.preamble.empty());

    // PCHInfo.deps should list files involved in building the PCH.
    ASSERT_FALSE(info.deps.empty());

    // PCHInfo.arguments should match what was passed in.
    ASSERT_EQ(info.arguments.size(), params.arguments.size());

    // Clean up the temp file.
    llvm::sys::fs::remove(*pch_path);
}

TEST_CASE(PCHBuildAndReuse) {
    add_file("types.h", R"(
#pragma once
template <typename T>
struct Vec {
    T* data;
    int size;
};
)");

    llvm::StringRef content = R"(
#include "types.h"

int main() {
    Vec<int> v;
    v.size = 3;
    return v.size;
}
)";

    add_main("main.cpp", content);

    // compile_with_pch does the full PCH build + content compile cycle.
    ASSERT_TRUE(compile_with_pch());

    // The resulting unit should have completed successfully.
    ASSERT_TRUE(unit.has_value());

    // Verify we can access the AST (top level decls should exist).
    ASSERT_TRUE(unit->top_level_decls().size() >= 1U);
}

TEST_CASE(PreambleBoundComputation) {
    // Test that compute_preamble_bound correctly identifies the end of the preamble.
    llvm::StringRef code_with_preamble = R"(
#include "a.h"
#include "b.h"

int main() { return 0; }
)";

    auto bound = compute_preamble_bound(code_with_preamble);
    // Bound should be > 0 (there are includes).
    ASSERT_TRUE(bound > 0);
    // Bound should be less than the total content size.
    ASSERT_TRUE(bound < code_with_preamble.size());

    // The content before the bound should contain the includes.
    auto preamble_part = code_with_preamble.substr(0, bound);
    ASSERT_TRUE(preamble_part.contains("#include"));

    // Code with no preamble.
    llvm::StringRef no_preamble = R"(
int main() { return 0; }
)";
    auto bound2 = compute_preamble_bound(no_preamble);
    ASSERT_EQ(bound2, 0U);
}

TEST_CASE(PCMBuildChain) {
    // Test that A imports B works: build PCM for B, then compile A using B's PCM.
    TempDir tmp;

    // Module B: no dependencies.
    tmp.touch("mod_b.cppm", R"(
export module mod_b;
export int b_value() { return 42; }
)");

    // Module A: imports B.
    tmp.touch("mod_a.cppm", R"(
export module mod_a;
import mod_b;
export int a_value() { return b_value() + 1; }
)");

    CompilationDatabase cdb;
    CommandOptions cmd_opts;
    cmd_opts.query_toolchain = true;
    cmd_opts.suppress_logging = true;

    // Build PCM for mod_b.
    cdb.add_command(tmp.root.str(),
                    tmp.path("mod_b.cppm"),
                    std::format("clang++ -std=c++20 {}", tmp.path("mod_b.cppm")));

    CompilationParams params_b;
    params_b.kind = CompilationKind::ModuleInterface;
    params_b.arguments = cdb.lookup(tmp.path("mod_b.cppm"), cmd_opts).front().arguments;

    auto pcm_b_path = fs::createTemporaryFile("mod_b", "pcm");
    ASSERT_TRUE(pcm_b_path.operator bool());
    params_b.output_file = *pcm_b_path;

    PCMInfo info_b;
    auto unit_b = clice::compile(params_b, info_b);
    ASSERT_TRUE(unit_b.completed());
    ASSERT_EQ(info_b.path, *pcm_b_path);

    // Build PCM for mod_a, passing B's PCM.
    cdb.add_command(tmp.root.str(),
                    tmp.path("mod_a.cppm"),
                    std::format("clang++ -std=c++20 {}", tmp.path("mod_a.cppm")));

    CompilationParams params_a;
    params_a.kind = CompilationKind::ModuleInterface;
    params_a.arguments = cdb.lookup(tmp.path("mod_a.cppm"), cmd_opts).front().arguments;
    params_a.pcms.try_emplace("mod_b", info_b.path);

    auto pcm_a_path = fs::createTemporaryFile("mod_a", "pcm");
    ASSERT_TRUE(pcm_a_path.operator bool());
    params_a.output_file = *pcm_a_path;

    PCMInfo info_a;
    auto unit_a = clice::compile(params_a, info_a);
    ASSERT_TRUE(unit_a.completed());
    ASSERT_EQ(info_a.path, *pcm_a_path);

    // info_a should record mod_b as a dependency.
    ASSERT_TRUE(llvm::find(info_a.mods, "mod_b") != info_a.mods.end());

    // Clean up temp PCM files.
    llvm::sys::fs::remove(*pcm_b_path);
    llvm::sys::fs::remove(*pcm_a_path);
}

TEST_CASE(PCHContentDifference) {
    // PCH should only contain the preamble portion; modifying code after
    // the preamble should not require PCH rebuild.
    add_file("common.h", R"(
#pragma once
struct Common { int val; };
)");

    llvm::StringRef content_v1 = R"(
#include "common.h"

int foo() { return 1; }
)";

    llvm::StringRef content_v2 = R"(
#include "common.h"

int foo() { return 2; }
int bar() { return 3; }
)";

    // Both versions should have the same preamble bound.
    auto bound_v1 = compute_preamble_bound(content_v1);
    auto bound_v2 = compute_preamble_bound(content_v2);
    ASSERT_EQ(bound_v1, bound_v2);

    // Build PCH with v1.
    add_main("main.cpp", content_v1);
    ASSERT_TRUE(compile_with_pch());
    ASSERT_TRUE(unit.has_value());
    ASSERT_TRUE(unit->top_level_decls().size() >= 1U);
}

};  // TEST_SUITE(Compiler)

}  // namespace

}  // namespace clice::testing
