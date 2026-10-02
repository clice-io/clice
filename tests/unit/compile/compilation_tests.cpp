#include <algorithm>
#include <format>
#include <thread>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "syntax/scan.h"
#include "vfs/path.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {

namespace {

ZEST_SUITE(Compiler, Tester) {

ZEST_CASE(TopLevelDecls) {
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
    ASSERT(compile_with_pch());
    ASSERT(unit->top_level_decls().size() == 4U);

}  // namespace

ZEST_CASE(DirectoryAnchorsIncludes) {
    /// The entry's `directory` governs relative search paths in the
    /// compile: -Igen must resolve against it, not the process cwd.
    TempDir tmp;
    tmp.touch("gen/config.h", "#define FROM_GEN 1\n");
    tmp.touch("main.cpp", R"(
#include <config.h>
int x = FROM_GEN;
)");

    std::vector<std::string> owned = {"clang++", "-std=c++20", "-Igen", tmp.path("main.cpp")};
    for(auto& arg: owned) {
        params.arguments.push_back(arg.c_str());
    }
    params.directory = tmp.root.str().str();

    auto built = clice::compile(params);
    ASSERT(built.completed());
    ASSERT(built.diagnostics().empty());
}

ZEST_CASE(OverlayNamesRedirectedFile) {
    /// A header an -ivfsoverlay maps under a virtual name is a dependency
    /// under the name of the file actually read.
    TempDir tmp;
    tmp.touch("real/config.h", "#define FROM_REAL 1\n");
    tmp.touch("main.cpp", "#include <config.h>\nint x = FROM_REAL;\n");
    auto overlay = std::format(R"({{
  "version": 0,
  "use-external-names": false,
  "roots": [{{
    "name": "{}",
    "type": "directory",
    "contents": [{{ "name": "config.h", "type": "file", "external-contents": "{}" }}]
  }}]
}})",
                               path::convert_to_slash(tmp.path("virtual")),
                               path::convert_to_slash(tmp.path("real/config.h")));
    tmp.touch("overlay.yaml", overlay);

    std::vector<std::string> owned = {"clang++",
                                      "-std=c++20",
                                      "-ivfsoverlay",
                                      tmp.path("overlay.yaml"),
                                      "-I" + tmp.path("virtual"),
                                      tmp.path("main.cpp")};
    for(auto& arg: owned) {
        params.arguments.push_back(arg.c_str());
    }
    params.directory = tmp.root.str().str();

    auto built = clice::compile(params);
    ASSERT(built.completed());
    ASSERT(built.diagnostics().empty());
    auto real = CanonicalPath(Spelling::absolute(tmp.path("real/config.h"))).str();
    EXPECT(llvm::any_of(built.deps(), [&](const DepFile& dep) { return dep.path == real; }));
}

ZEST_CASE(StopCompilation) {
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
    ASSERT(!built.completed());
    // Pinned distinctly from setup_fail: the worker maps this status to
    // CompileStatus::Cancelled, which the master discards without blaming
    // any artifact.
    ASSERT(built.cancelled());
}

ZEST_CASE(PCHBuildPopulatesInfo) {
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

    auto pch_path = vfs::temp_file("clice-test", "pch");
    ASSERT(pch_path.operator bool());
    params.output_file = *pch_path;

    // Add truncated main file buffer for preamble build.
    auto& source = sources.all_files["main.cpp"];
    auto bound = compute_preamble_bound(source.content);
    auto main_vfs_path = TestVFS::path("main.cpp");
    params.add_remapped_file(main_vfs_path, source.content, bound);

    PCHInfo info;
    auto preamble_unit = clice::compile(params, info);
    ASSERT(preamble_unit.completed());

    // PCHInfo.path should match the output file.
    ASSERT(info.path == *pch_path);

    // build_at is sampled before the compile runs (non-zero, recent).
    ASSERT(preamble_unit.build_at().count() > 0);

    // PCHInfo.preamble should be non-empty (contains the #include directives).
    ASSERT(!info.preamble.empty());

    // PCHInfo.deps should list files involved in building the PCH, each with
    // the hash of the consumed bytes.
    ASSERT(!info.deps.empty());
    for(auto& dep: info.deps) {
        ASSERT(!dep.path.empty());
        ASSERT(dep.hash != 0);
    }

    // PCHInfo.arguments should match what was passed in.
    ASSERT(info.arguments.size() == params.arguments.size());

    // Clean up the temp file.
    llvm::sys::fs::remove(*pch_path);
}

ZEST_CASE(CorruptPCHAttributable) {
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
    auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        llvm::makeIntrusiveRefCnt<vfs::View>());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    auto pch_path = vfs::temp_file("clice-test", "pch");
    ASSERT(pch_path.operator bool());

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
            ASSERT(preamble_unit.completed());
        }

        auto blob = read_file(*pch_path);
        ASSERT(blob.operator bool());
        ASSERT(!vfs::write(*pch_path, corrupt(std::move(*blob), shape)));

        params.kind = CompilationKind::Content;
        params.output_file.clear();
        params.pch = {*pch_path, static_cast<std::uint32_t>(info.preamble.size())};
        params.buffers.clear();

        auto content_unit = clice::compile(params);
        ASSERT(!content_unit.completed());
        ASSERT((content_unit.setup_fail() || content_unit.fatal_error()));
        // The blame signal the master's retraction keys on (pch_suspect):
        // a diagnostic naming the blob, or an AST-deserialization error —
        // that family's messages do not reliably carry the path ("Blob
        // ends too soon").
        bool blames_pch = std::ranges::any_of(content_unit.diagnostics(), [&](auto& diag) {
            return llvm::StringRef(diag.message).contains(*pch_path) ||
                   diag.id.is_deserialization_error();
        });
        ASSERT(blames_pch);
    }

    llvm::sys::fs::remove(*pch_path);
}

ZEST_CASE(PCHIgnoresInputMtime) {
    TempDir tmp;
    tmp.touch("preamble.h", "int preamble_func();\n");
    auto header = tmp.path("preamble.h");
    auto content =
        std::format("#include \"{}\"\n\nint main() {{ return preamble_func(); }}\n", header);
    add_main("main.cpp", content);
    prepare();

    auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        llvm::makeIntrusiveRefCnt<vfs::View>());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    auto pch_path = vfs::temp_file("clice-test", "pch");
    ASSERT(pch_path.operator bool());
    auto main_vfs_path = TestVFS::path("main.cpp");
    auto bound = compute_preamble_bound(content);

    params.kind = CompilationKind::Preamble;
    params.output_file = *pch_path;
    params.add_remapped_file(main_vfs_path, content, bound);
    PCHInfo info;
    ASSERT(clice::compile(params, info).completed());

    auto compile_with = [&] {
        params.kind = CompilationKind::Content;
        params.output_file.clear();
        params.pch = {*pch_path, bound};
        params.buffers.clear();
        return clice::compile(params);
    };

    // A same-bytes rewrite moves only the mtime.
    ASSERT(set_file_mtime(header, file_mtime_ns(header) + 10'000'000'000));
    {
        auto unit = compile_with();
        ASSERT(unit.completed());
        ASSERT(std::ranges::none_of(unit.diagnostics(), [](auto& diag) {
            return diag.id.level >= DiagnosticLevel::Error;
        }));
    }

    // A size change still rejects it.
    tmp.touch("preamble.h", "int preamble_func();\nint more;\n");
    ASSERT(!compile_with().completed());

    llvm::sys::fs::remove(*pch_path);
}

ZEST_CASE(PCHBuildAndReuse) {
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
    ASSERT(compile_with_pch());

    // The resulting unit should have completed successfully.
    ASSERT(unit);

    // Verify we can access the AST (top level decls should exist).
    ASSERT(unit->top_level_decls().size() >= 1U);
}

ZEST_CASE(PreambleBoundComputation) {
    // Test that compute_preamble_bound correctly identifies the end of the preamble.
    llvm::StringRef code_with_preamble = R"(
#include "a.h"
#include "b.h"

int main() { return 0; }
)";

    auto bound = compute_preamble_bound(code_with_preamble);
    // Bound should be > 0 (there are includes).
    ASSERT(bound > 0);
    // Bound should be less than the total content size.
    ASSERT(bound < code_with_preamble.size());

    // The content before the bound should contain the includes.
    auto preamble_part = code_with_preamble.substr(0, bound);
    ASSERT(preamble_part.contains("#include"));

    // Code with no preamble.
    llvm::StringRef no_preamble = R"(
int main() { return 0; }
)";
    auto bound2 = compute_preamble_bound(no_preamble);
    ASSERT(bound2 == 0U);
}

ZEST_CASE(PCMBuildChain) {
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

    FileTable file_table;
    CompilationDatabase cdb{file_table};

    auto render_entry = [&](llvm::StringRef file) {
        auto& entry = cdb.candidate_entries(file).front();
        CommandRef ref{entry.file,
                       entry.config,
                       cdb.input_kind(entry.config, file),
                       CommandSource::CDBExact};
        EXPECT(cdb.toolchain().resolve(ref.config, ref.input));
        return cdb.render(ref);
    };

    // Build PCM for mod_b.
    cdb.add_command(tmp.root.str(),
                    tmp.path("mod_b.cppm"),
                    std::format("clang++ -std=c++20 {}", tmp.path("mod_b.cppm")));

    CompilationParams params_b;
    params_b.kind = CompilationKind::ModuleInterface;
    params_b.arguments = render_entry(tmp.path("mod_b.cppm"));

    auto pcm_b_path = vfs::temp_file("mod_b", "pcm");
    ASSERT(pcm_b_path.operator bool());
    params_b.output_file = *pcm_b_path;

    PCMInfo info_b;
    auto unit_b = clice::compile(params_b, info_b);
    ASSERT(unit_b.completed());
    ASSERT(info_b.path == *pcm_b_path);

    // Build PCM for mod_a, passing B's PCM.
    cdb.add_command(tmp.root.str(),
                    tmp.path("mod_a.cppm"),
                    std::format("clang++ -std=c++20 {}", tmp.path("mod_a.cppm")));

    CompilationParams params_a;
    params_a.kind = CompilationKind::ModuleInterface;
    params_a.arguments = render_entry(tmp.path("mod_a.cppm"));
    params_a.pcms.try_emplace("mod_b", info_b.path);

    auto pcm_a_path = vfs::temp_file("mod_a", "pcm");
    ASSERT(pcm_a_path.operator bool());
    params_a.output_file = *pcm_a_path;

    PCMInfo info_a;
    auto unit_a = clice::compile(params_a, info_a);
    ASSERT(unit_a.completed());
    ASSERT(info_a.path == *pcm_a_path);

    // info_a should record mod_b as a dependency.
    ASSERT(llvm::find(info_a.mods, "mod_b") != info_a.mods.end());

    // Clean up temp PCM files.
    llvm::sys::fs::remove(*pcm_b_path);
    llvm::sys::fs::remove(*pcm_a_path);
}

ZEST_CASE(PCHContentDifference) {
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
    ASSERT(bound_v1 == bound_v2);

    // Build PCH with v1.
    add_main("main.cpp", content_v1);
    ASSERT(compile_with_pch());
    ASSERT(unit);
    ASSERT(unit->top_level_decls().size() >= 1U);
}

};  // ZEST_SUITE(Compiler)

ZEST_SUITE(PreambleHash) {

ZEST_CASE(StableForBodyChanges) {
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
    EXPECT(bound1 == bound2);

    auto hash1 = llvm::xxh3_64bits(v1.substr(0, bound1));
    auto hash2 = llvm::xxh3_64bits(v2.substr(0, bound2));
    EXPECT(hash1 == hash2);
}

ZEST_CASE(ChangesForNewInclude) {
    // Different preamble (#include added) → different hash → PCH must rebuild.
    llvm::StringRef v1 = R"cpp(
#include "a.h"
int x = 1;
)cpp";
    llvm::StringRef v2 = R"cpp(
#include "a.h"
#include "b.h"
#include "vfs/file_system.h"
int x = 1;
)cpp";

    auto bound1 = compute_preamble_bound(v1);
    auto bound2 = compute_preamble_bound(v2);
    EXPECT(bound1 != bound2);

    auto hash1 = llvm::xxh3_64bits(v1.substr(0, bound1));
    auto hash2 = llvm::xxh3_64bits(v2.substr(0, bound2));
    EXPECT(hash1 != hash2);
}

ZEST_CASE(ZeroBoundNoPCH) {
    // No preprocessor directives → bound is 0 → PCH should be skipped.
    llvm::StringRef code = R"cpp(
int main() { return 0; }
)cpp";

    auto bound = compute_preamble_bound(code);
    EXPECT(bound == 0u);
}

};  // ZEST_SUITE(PreambleHash)

}  // namespace

}  // namespace clice::testing
