#include <algorithm>
#include <thread>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "support/filesystem.h"
#include "syntax/scan.h"

#include "llvm/Support/xxhash.h"

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
    // Pinned distinctly from setup_fail: the worker maps this status to
    // CompileStatus::Cancelled, which the master discards without blaming
    // any artifact.
    ASSERT_TRUE(built.cancelled());
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

    // build_at is sampled before the compile runs (non-zero, recent).
    ASSERT_TRUE(preamble_unit.build_at().count() > 0);

    // PCHInfo.preamble should be non-empty (contains the #include directives).
    ASSERT_FALSE(info.preamble.empty());

    // PCHInfo.deps should list files involved in building the PCH, each with
    // the hash of the consumed bytes.
    ASSERT_FALSE(info.deps.empty());
    for(auto& dep: info.deps) {
        ASSERT_FALSE(dep.path.empty());
        ASSERT_TRUE(dep.hash != 0);
    }

    // PCHInfo.arguments should match what was passed in.
    ASSERT_EQ(info.arguments.size(), params.arguments.size());

    // Clean up the temp file.
    llvm::sys::fs::remove(*pch_path);
}

TEST_CASE(CorruptPCHAttributable) {
    add_file("preamble.h", R"(
#pragma once
int preamble_func();
)");

    llvm::StringRef content = R"(
#include "preamble.h"

int main() { return preamble_func(); }
)";
    add_main("main.cpp", content);
    prepare();

    // Consuming the PCH reads it from real disk; overlay like compile_with_pch.
    auto overlay =
        llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    auto pch_path = fs::createTemporaryFile("clice-test", "pch");
    ASSERT_TRUE(pch_path.operator bool());

    auto& source = sources.all_files["main.cpp"];
    auto bound = compute_preamble_bound(source.content);
    auto main_vfs_path = TestVFS::path("main.cpp");

    // Corruption the reader detects in-process must stay attributable to
    // the artifact — a setup failure, or a fatal error naming the blob —
    // never a completed parse: that contract is what the master's quality
    // gate (retract the pair and rebuild) keys on. Whole-file garbage is
    // rejected by validation; truncation is caught reading the block
    // structure. Mid-file bit flips can instead abort the process inside
    // the bitstream reader (report_fatal_error), which in production
    // kills the worker — that shape is exercised end-to-end by the
    // integration corruption tests, not here.
    auto corrupt = [](std::string blob, std::size_t shape) -> std::string {
        return shape == 0 ? std::string(blob.size(), '\x5A')  // whole-file garbage
                          : blob.substr(0, blob.size() / 2);  // truncation
    };

    for(std::size_t shape = 0; shape < 2; ++shape) {
        params.kind = CompilationKind::Preamble;
        params.output_file = *pch_path;
        params.pch = {};
        params.buffers.clear();
        params.add_remapped_file(main_vfs_path, source.content, bound);

        PCHInfo info;
        {
            auto preamble_unit = clice::compile(params, info);
            ASSERT_TRUE(preamble_unit.completed());
        }

        auto blob = fs::read(*pch_path);
        ASSERT_TRUE(blob.operator bool());
        ASSERT_TRUE(fs::write(*pch_path, corrupt(std::move(*blob), shape)).operator bool());

        params.kind = CompilationKind::Content;
        params.output_file.clear();
        params.pch = {*pch_path, static_cast<std::uint32_t>(info.preamble.size())};
        params.buffers.clear();

        auto content_unit = clice::compile(params);
        ASSERT_FALSE(content_unit.completed());
        ASSERT_TRUE(content_unit.setup_fail() || content_unit.fatal_error());
        // The blame signal the master's retraction keys on (pch_suspect):
        // a diagnostic naming the blob, or an AST-deserialization error —
        // that family's messages do not reliably carry the path ("Blob
        // ends too soon").
        bool blames_pch = std::ranges::any_of(content_unit.diagnostics(), [&](auto& diag) {
            return llvm::StringRef(diag.message).contains(*pch_path) ||
                   diag.id.is_deserialization_error();
        });
        ASSERT_TRUE(blames_pch);
    }

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
    Toolchain tc;

    // Build PCM for mod_b.
    cdb.add_command(tmp.root.str(),
                    tmp.path("mod_b.cppm"),
                    std::format("clang++ -std=c++20 {}", tmp.path("mod_b.cppm")));

    auto cmds_b = cdb.lookup(tmp.path("mod_b.cppm"));
    ASSERT_TRUE(tc.resolve(cmds_b.front()).has_value());
    CompilationParams params_b;
    params_b.kind = CompilationKind::ModuleInterface;
    params_b.arguments = cmds_b.front().to_argv();

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

    auto cmds_a = cdb.lookup(tmp.path("mod_a.cppm"));
    ASSERT_TRUE(tc.resolve(cmds_a.front()).has_value());
    CompilationParams params_a;
    params_a.kind = CompilationKind::ModuleInterface;
    params_a.arguments = cmds_a.front().to_argv();
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

TEST_SUITE(PreambleHash) {

TEST_CASE(StableForBodyChanges) {
    // Same preamble (#include lines) but different body → same hash → PCH reusable.
    llvm::StringRef v1 = R"cpp(
#include "a.h"
#include "b.h"
int x = 1;
)cpp";
    llvm::StringRef v2 = R"cpp(
#include "a.h"
#include "b.h"
int x = 2;
void foo() {}
)cpp";

    auto bound1 = compute_preamble_bound(v1);
    auto bound2 = compute_preamble_bound(v2);
    EXPECT_EQ(bound1, bound2);

    auto hash1 = llvm::xxh3_64bits(v1.substr(0, bound1));
    auto hash2 = llvm::xxh3_64bits(v2.substr(0, bound2));
    EXPECT_EQ(hash1, hash2);
}

TEST_CASE(ChangesForNewInclude) {
    // Different preamble (#include added) → different hash → PCH must rebuild.
    llvm::StringRef v1 = R"cpp(
#include "a.h"
int x = 1;
)cpp";
    llvm::StringRef v2 = R"cpp(
#include "a.h"
#include "b.h"
int x = 1;
)cpp";

    auto bound1 = compute_preamble_bound(v1);
    auto bound2 = compute_preamble_bound(v2);
    EXPECT_NE(bound1, bound2);

    auto hash1 = llvm::xxh3_64bits(v1.substr(0, bound1));
    auto hash2 = llvm::xxh3_64bits(v2.substr(0, bound2));
    EXPECT_NE(hash1, hash2);
}

TEST_CASE(ZeroBoundNoPCH) {
    // No preprocessor directives → bound is 0 → PCH should be skipped.
    llvm::StringRef code = R"cpp(
int main() { return 0; }
)cpp";

    auto bound = compute_preamble_bound(code);
    EXPECT_EQ(bound, 0u);
}

};  // TEST_SUITE(PreambleHash)

}  // namespace

}  // namespace clice::testing
