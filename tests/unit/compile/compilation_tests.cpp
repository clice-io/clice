#include <thread>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "index/tu_index.h"
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

    // PCHInfo.deps should list files involved in building the PCH, each
    // hashed from the buffer the compiler consumed.
    ASSERT_FALSE(info.deps.empty());
    auto dep = llvm::find_if(info.deps, [](const HashedDep& dep) {
        return llvm::StringRef(dep.path).ends_with("preamble.h");
    });
    ASSERT_TRUE(dep != info.deps.end());
    ASSERT_EQ(dep->hash, llvm::xxh3_64bits(sources.all_files["preamble.h"].content));

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
    llvm::StringRef b_content = R"(
export module mod_b;
export int b_value() { return 42; }
)";
    tmp.touch("mod_b.cppm", b_content);

    // Module A: imports B.
    llvm::StringRef a_content = R"(
export module mod_a;
import mod_b;
export int a_value() { return b_value() + 1; }
)";
    tmp.touch("mod_a.cppm", a_content);

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

    // Each PCM's deps carry its own interface source, and A's also carry
    // B's interface source (the import edge), all with content hashes.
    // Compare separator-normalized: on Windows the driver records forward
    // slashes while TempDir hands out native paths, and the consuming
    // staleness checks are stat-based, so either form is functional.
    auto has_dep = [](const PCMInfo& info, llvm::StringRef path, std::uint64_t hash) {
        return llvm::any_of(info.deps, [&](const HashedDep& dep) {
            return llvm::sys::path::convert_to_slash(dep.path) ==
                       llvm::sys::path::convert_to_slash(path) &&
                   dep.hash == hash;
        });
    };
    ASSERT_TRUE(has_dep(info_b, tmp.path("mod_b.cppm"), llvm::xxh3_64bits(b_content)));
    ASSERT_TRUE(has_dep(info_a, tmp.path("mod_a.cppm"), llvm::xxh3_64bits(a_content)));
    ASSERT_TRUE(has_dep(info_a, tmp.path("mod_b.cppm"), llvm::xxh3_64bits(b_content)));

    // Clean up temp PCM files.
    llvm::sys::fs::remove(*pcm_b_path);
    llvm::sys::fs::remove(*pcm_a_path);
}

TEST_CASE(IndexImporterTUIndex) {
    // Index-kind compile of a TU importing a module: the TUIndex must carry
    // the interface source as an import dependency and serialize cleanly.
    TempDir tmp;

    llvm::StringRef iface_content = R"(
export module mod_x;
export inline int x_value() { return 42; }
)";
    tmp.touch("mod_x.cppm", iface_content);
    tmp.touch("user.cpp", R"(
import mod_x;
int use_x() { return x_value(); }
)");

    CompilationDatabase cdb;
    Toolchain tc;

    cdb.add_command(tmp.root.str(),
                    tmp.path("mod_x.cppm"),
                    std::format("clang++ -std=c++20 {}", tmp.path("mod_x.cppm")));
    auto cmds_x = cdb.lookup(tmp.path("mod_x.cppm"));
    ASSERT_TRUE(tc.resolve(cmds_x.front()).has_value());
    CompilationParams params_x;
    params_x.kind = CompilationKind::ModuleInterface;
    params_x.arguments = cmds_x.front().to_argv();

    auto pcm_path = fs::createTemporaryFile("mod_x", "pcm");
    ASSERT_TRUE(pcm_path.operator bool());
    params_x.output_file = *pcm_path;

    PCMInfo info_x;
    auto unit_x = clice::compile(params_x, info_x);
    ASSERT_TRUE(unit_x.completed());

    cdb.add_command(tmp.root.str(),
                    tmp.path("user.cpp"),
                    std::format("clang++ -std=c++20 {}", tmp.path("user.cpp")));
    auto cmds_u = cdb.lookup(tmp.path("user.cpp"));
    ASSERT_TRUE(tc.resolve(cmds_u.front()).has_value());
    CompilationParams params_u;
    params_u.kind = CompilationKind::Indexing;
    params_u.arguments = cmds_u.front().to_argv();
    params_u.pcms.try_emplace("mod_x", *pcm_path);

    auto unit_u = clice::compile(params_u);
    ASSERT_TRUE(unit_u.completed());

    auto tu_index = index::TUIndex::build(unit_u);
    ASSERT_EQ(tu_index.imports.size(), std::size_t(1));
    auto import_id = tu_index.imports[0];
    ASSERT_EQ(tu_index.graph.paths[import_id], tmp.path("mod_x.cppm"));
    ASSERT_EQ(tu_index.graph.path_hashes[import_id], llvm::xxh3_64bits(iface_content));

    // The module symbol must have the same identity on both sides of the
    // PCM boundary, or cross-TU references could never connect.
    auto iface_index = index::TUIndex::build(unit_x);
    auto find_symbol = [](const index::TUIndex& idx, llvm::StringRef name) -> std::uint64_t {
        for(auto& [hash, symbol]: idx.symbols) {
            if(symbol.name == name) {
                return hash;
            }
        }
        return 0;
    };
    auto hash_in_iface = find_symbol(iface_index, "x_value");
    auto hash_in_user = find_symbol(tu_index, "x_value");
    ASSERT_TRUE(hash_in_iface != 0);
    ASSERT_EQ(hash_in_iface, hash_in_user);

    // Round-trip through the wire format, as the indexer receives it.
    std::string data;
    llvm::raw_string_ostream os(data);
    tu_index.serialize(os);
    auto restored = index::TUIndex::from(data.data());
    ASSERT_EQ(restored.imports, tu_index.imports);
    ASSERT_EQ(restored.graph.path_hashes, tu_index.graph.path_hashes);

    // Rows located inside the imported module's own files belong to the
    // module's index run, not this TU's: the import edge is a dependency
    // only, never a contribution.
    ASSERT_FALSE(restored.path_file_indices.contains(import_id));

    llvm::sys::fs::remove(*pcm_path);
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
