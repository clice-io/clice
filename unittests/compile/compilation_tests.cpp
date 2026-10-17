module;

#include "modules/prelude.h"

module clice;

import :command.command;
import :command.toolchain;
import :compile.compilation;
import :syntax.scan;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :unittests.utils.tester;
import :vfs.path;

namespace clice::testing {

namespace {

/// The C++20 command of `file` in `tmp`, added to `cdb` and rendered.
std::vector<std::string> cxx20_command(CompilationDatabase& cdb,
                                       const TempDir& tmp,
                                       llvm::StringRef file) {
    auto path = tmp.path(file);
    cdb.add_command(tmp.root.str(), path, std::format("clang++ -std=c++20 {}", path));
    auto& entry = cdb.candidate_entries(path).front();
    CommandRef ref{entry.file,
                   entry.config,
                   cdb.input_kind(entry.config, path),
                   CommandSource::CDBExact};
    ZEXPECT(cdb.toolchain().resolve(ref.config, ref.input));
    auto argv = cdb.render(ref);
    return std::vector<std::string>(argv.begin(), argv.end());
}

/// Whether the unit defines the one specialization of the function
/// template `name` its code uses.
bool defines_specialization(CompilationUnit& unit, llvm::StringRef name) {
    auto found = unit.tu()->lookup(&unit.context().Idents.get(name));
    auto* pattern = llvm::dyn_cast<clang::FunctionTemplateDecl>(found.front());
    ZASSERT(pattern);
    ZASSERT(llvm::range_size(pattern->specializations()) == 1U);
    return (*pattern->specializations().begin())->isDefined();
}

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
    ZASSERT(compile_with_pch());
    ZASSERT(unit->top_level_decls().size() == 4U);
}

ZEST_CASE(DirectoryAnchorsIncludes) {
    /// The entry's `directory` governs relative search paths in the
    /// compile: -Igen must resolve against it, not the process cwd.
    TempDir tmp;
    tmp.touch("gen/config.h", "#define FROM_GEN 1\n");
    tmp.touch("main.cpp", R"(
#include <config.h>
int x = FROM_GEN;
)");

    params.kind = CompilationKind::Content;
    params.input.arguments = {"clang++", "-std=c++20", "-Igen", tmp.path("main.cpp")};
    params.input.directory = tmp.root.str().str();

    auto built = clice::compile(std::move(params));
    ZASSERT(built->completed());
    ZASSERT(built->diagnostics().empty());
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

    params.kind = CompilationKind::Content;
    params.input.arguments = {"clang++",
                              "-std=c++20",
                              "-ivfsoverlay",
                              tmp.path("overlay.yaml"),
                              "-I" + tmp.path("virtual"),
                              tmp.path("main.cpp")};
    params.input.directory = tmp.root.str().str();

    auto built = clice::compile(std::move(params));
    ZASSERT(built->completed());
    ZASSERT(built->diagnostics().empty());
    auto real = CanonicalPath(Spelling::absolute(tmp.path("real/config.h"))).str();
    ZEXPECT(llvm::any_of(built->deps(), [&](const DepFile& dep) { return dep.path == real; }));
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

    auto built = clice::compile(std::move(params));
    ZASSERT(!built->completed());
    // Pinned distinctly from setup_fail: the worker maps this status to
    // CompileStatus::Cancelled, which the master discards without blaming
    // any artifact.
    ZASSERT(!built->setup_fail());
}

ZEST_CASE(NoErrorLimit) {
    std::string content;
    for(int i = 0; i < 25; i += 1) {
        content += std::format("int f{0}() {{ return undefined_{0}; }}\n", i);
    }
    add_main("main.cpp", content);
    prepare();
    // The driver puts its default limit on every command.
    params.input.arguments.insert(params.input.arguments.end() - 1, {"-ferror-limit", "19"});

    ZASSERT(try_compile());
    ZEXPECT(!unit->fatal_error());
    ZEXPECT(collect_errors(*unit).size() == 25);
}

ZEST_CASE(FatalError) {
    add_main("main.cpp", R"(
import missing;
int x = 1;
)");
    ZASSERT(compile());
    ZEXPECT(unit->fatal_error());

    clear();
    add_main("main.cpp", "int x = 1;\n");
    ZASSERT(compile());
    ZEXPECT(!unit->fatal_error());
}

ZEST_CASE(PCHBuild) {
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
    ZASSERT(pch_path.operator bool());
    params.output_file = *pch_path;

    // Add truncated main file buffer for preamble build.
    auto& source = sources.all_files["main.cpp"];
    auto bound = compute_preamble_bound(source.content);
    auto main_vfs_path = TestVFS::path("main.cpp");
    params.input.add_remapped_file(main_vfs_path, source.content, bound);

    auto preamble_unit = clice::compile(std::move(params));
    ZASSERT(preamble_unit->completed());
    ZASSERT(preamble_unit->has_output());

    // build_at is sampled before the compile runs (non-zero, recent).
    ZASSERT(preamble_unit->build_at().count() > 0);

    // The deps should list files involved in building the PCH, each with
    // the hash of the consumed bytes.
    auto deps = preamble_unit->deps();
    ZASSERT(!deps.empty());
    for(auto& dep: deps) {
        ZASSERT(!dep.path.empty());
        ZASSERT(dep.hash != 0);
    }

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
    params.input.vfs = overlay;

    auto pch_path = vfs::temp_file("clice-test", "pch");
    ZASSERT(pch_path.operator bool());

    auto& source = sources.all_files["main.cpp"];
    auto bound = compute_preamble_bound(source.content);
    auto main_vfs_path = TestVFS::path("main.cpp");

    // Corruption the reader detects in-process must stay attributable to
    // the PCH — a setup failure, or a parse cut short by a fatal error
    // naming the blob — never a clean parse: that contract is what the
    // master's quality gate (retract the pair and rebuild) keys on.
    // Whole-file garbage is rejected by validation; truncation is caught
    // reading the block structure. Mid-file bit flips can instead abort the
    // process inside the bitstream reader (report_fatal_error), which in
    // production kills the worker — that shape is exercised end-to-end by
    // the integration corruption tests, not here.
    auto corrupt = [](std::string blob, std::size_t shape) -> std::string {
        return shape == 0 ? std::string(blob.size(), '\x5A')  // whole-file garbage
                          : blob.substr(0, blob.size() / 2);  // truncation
    };

    for(std::size_t shape = 0; shape < 2; ++shape) {
        CompilationParams preamble{
            .kind = CompilationKind::Preamble,
            .input = params.input,
            .output_file = *pch_path,
        };
        preamble.input.add_remapped_file(main_vfs_path, source.content, bound);
        {
            auto preamble_unit = clice::compile(std::move(preamble));
            ZASSERT(preamble_unit->has_output());
        }

        auto blob = read_file(*pch_path);
        ZASSERT(blob.operator bool());
        ZASSERT(!vfs::write(*pch_path, corrupt(std::move(*blob), shape)));

        CompilationParams reuse{.kind = CompilationKind::Content, .input = params.input};
        reuse.input.pch = {*pch_path, bound};
        auto content_unit = clice::compile(std::move(reuse));
        ZASSERT((content_unit->setup_fail() || content_unit->fatal_error()));
        // The blame signal the master's retraction keys on (pch_suspect):
        // a diagnostic naming the blob, or an AST-deserialization error —
        // that family's messages do not reliably carry the path ("Blob
        // ends too soon").
        bool blames_pch = std::ranges::any_of(content_unit->diagnostics(), [&](auto& diag) {
            return llvm::StringRef(diag.message).contains(*pch_path) ||
                   diag.id.is_deserialization_error();
        });
        ZASSERT(blames_pch);
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
    params.input.vfs = overlay;

    auto pch_path = vfs::temp_file("clice-test", "pch");
    ZASSERT(pch_path.operator bool());
    auto main_vfs_path = TestVFS::path("main.cpp");
    auto bound = compute_preamble_bound(content);

    CompilationParams preamble{
        .kind = CompilationKind::Preamble,
        .input = params.input,
        .output_file = *pch_path,
    };
    preamble.input.add_remapped_file(main_vfs_path, content, bound);
    ZASSERT(clice::compile(std::move(preamble))->has_output());

    auto compile_with = [&] {
        CompilationParams reuse{.kind = CompilationKind::Content, .input = params.input};
        reuse.input.pch = {*pch_path, bound};
        return clice::compile(std::move(reuse));
    };

    // A same-bytes rewrite moves only the mtime.
    ZASSERT(set_file_mtime(header, file_mtime_ns(header) + 10'000'000'000));
    {
        auto unit = compile_with();
        ZASSERT(unit->completed());
        ZASSERT(std::ranges::none_of(unit->diagnostics(), [](auto& diag) {
            return diag.id.level >= DiagnosticLevel::Error;
        }));
    }

    // A size change still rejects it.
    tmp.touch("preamble.h", "int preamble_func();\nint more;\n");
    ZASSERT(!compile_with()->completed());

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
    ZASSERT(compile_with_pch());

    // The resulting unit should have completed successfully.
    ZASSERT(unit);

    // Verify we can access the AST (top level decls should exist).
    ZASSERT(unit->top_level_decls().size() >= 1U);
}

ZEST_CASE(PCHKeepsPreambleErrors) {
    add_file("broken.h", R"(
#pragma once
int broken = undeclared_name;
int before();
)");
    add_file("after.h", R"(
#pragma once
int after();
)");

    // A missing include is fatal, and the preamble goes on past it.
    add_main("main.cpp", R"(
#include "broken.h"
#include "missing.h"
#include "after.h"

int main() { return before() + after(); }
)");

    ZASSERT(compile_with_pch());
    ZASSERT(std::ranges::none_of(unit->diagnostics(), [](auto& diag) {
        return diag.id.level >= DiagnosticLevel::Error;
    }));
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
    ZASSERT(bound > 0);
    // Bound should be less than the total content size.
    ZASSERT(bound < code_with_preamble.size());

    // The content before the bound should contain the includes.
    auto preamble_part = code_with_preamble.substr(0, bound);
    ZASSERT(preamble_part.contains("#include"));

    // Code with no preamble.
    llvm::StringRef no_preamble = R"(
int main() { return 0; }
)";
    auto bound2 = compute_preamble_bound(no_preamble);
    ZASSERT(bound2 == 0U);
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

    // Build PCM for mod_b.
    CompilationParams params_b;
    params_b.kind = CompilationKind::ModuleInterface;
    params_b.input.arguments = cxx20_command(cdb, tmp, "mod_b.cppm");

    auto pcm_b_path = vfs::temp_file("mod_b", "pcm");
    ZASSERT(pcm_b_path.operator bool());
    params_b.output_file = *pcm_b_path;

    auto unit_b = clice::compile(std::move(params_b));
    ZASSERT(unit_b->completed());
    ZASSERT(unit_b->has_output());

    // Build PCM for mod_a, passing B's PCM.
    CompilationParams params_a;
    params_a.kind = CompilationKind::ModuleInterface;
    params_a.input.arguments = cxx20_command(cdb, tmp, "mod_a.cppm");
    params_a.input.pcms.try_emplace("mod_b", *pcm_b_path);

    auto pcm_a_path = vfs::temp_file("mod_a", "pcm");
    ZASSERT(pcm_a_path.operator bool());
    params_a.output_file = *pcm_a_path;

    auto unit_a = clice::compile(std::move(params_a));
    ZASSERT(unit_a->completed());
    ZASSERT(unit_a->has_output());

    // Clean up temp PCM files.
    llvm::sys::fs::remove(*pcm_b_path);
    llvm::sys::fs::remove(*pcm_a_path);
}

ZEST_CASE(PCHKeepsModuleImports) {
    TempDir tmp;
    tmp.touch("b.cppm", "export module B;\nexport int b() { return 1; }\n");
    tmp.touch("pre.h", "import B;\n");
    llvm::StringRef content = "#include \"pre.h\"\nint use() { return b(); }\n";
    tmp.touch("main.cpp", content);

    FileTable file_table;
    CompilationDatabase cdb{file_table};

    CompilationParams module_params;
    module_params.kind = CompilationKind::ModuleInterface;
    module_params.input.arguments = cxx20_command(cdb, tmp, "b.cppm");
    auto pcm_path = vfs::temp_file("b", "pcm");
    ZASSERT(pcm_path.operator bool());
    module_params.output_file = *pcm_path;
    ZASSERT(clice::compile(std::move(module_params))->has_output());

    CompilationParams preamble;
    preamble.kind = CompilationKind::Preamble;
    preamble.input.arguments = cxx20_command(cdb, tmp, "main.cpp");
    preamble.input.pcms.try_emplace("B", *pcm_path);
    auto pch_path = vfs::temp_file("clice-test", "pch");
    ZASSERT(pch_path.operator bool());
    preamble.output_file = *pch_path;
    auto bound = compute_preamble_bound(content);

    CompilationParams params{.kind = CompilationKind::Content, .input = preamble.input};
    params.input.pch = {*pch_path, bound};

    preamble.input.add_remapped_file(tmp.path("main.cpp"), content, bound);
    ZASSERT(clice::compile(std::move(preamble))->has_output());

    auto unit = clice::compile(std::move(params));
    ZASSERT(unit->completed());
    ZASSERT(std::ranges::none_of(unit->diagnostics(), [](auto& diag) {
        return diag.id.level >= DiagnosticLevel::Error;
    }));

    llvm::sys::fs::remove(*pch_path);
    llvm::sys::fs::remove(*pcm_path);
}

ZEST_CASE(PCHWithoutModule) {
    TempDir tmp;
    tmp.touch("b.pcm", "not a module file");
    tmp.touch("pre.h", "import B;\n");
    llvm::StringRef content = "#include \"pre.h\"\nint use();\n";
    tmp.touch("main.cpp", content);

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    CompilationParams preamble{
        .kind = CompilationKind::Preamble,
        .input = {.arguments = cxx20_command(cdb, tmp, "main.cpp")},
    };
    preamble.input.pcms.try_emplace("B", tmp.path("b.pcm"));
    preamble.input.add_remapped_file(tmp.path("main.cpp"),
                                     content,
                                     compute_preamble_bound(content));
    auto pch_path = vfs::temp_file("clice-test", "pch");
    ZASSERT(pch_path.operator bool());
    preamble.output_file = *pch_path;

    // A preamble whose module fails to load gets no PCH.
    auto unit = clice::compile(std::move(preamble));
    ZASSERT(unit->completed());
    ZEXPECT(!unit->has_output());

    llvm::sys::fs::remove(*pch_path);
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
    ZASSERT(bound_v1 == bound_v2);

    // Build PCH with v1.
    add_main("main.cpp", content_v1);
    ZASSERT(compile_with_pch());
    ZASSERT(unit);
    ZASSERT(unit->top_level_decls().size() >= 1U);
}

/// The preamble build of `content` as main.cpp, writing its PCH to `pch`.
std::unique_ptr<CompilationUnit> build_preamble(llvm::StringRef content, llvm::StringRef pch) {
    add_main("main.cpp", content);
    prepare();
    params.kind = CompilationKind::Preamble;
    params.output_file = pch.str();
    params.input.add_remapped_file(TestVFS::path("main.cpp"),
                                   content,
                                   compute_preamble_bound(content));
    return clice::compile(std::move(params));
}

ZEST_CASE(PCHInstantiatesTemplates) {
    add_file("twice.h", R"(
#pragma once
template <typename T>
T twice(T value) {
    return value + value;
}

inline int four() {
    return twice(2);
}
)");

    auto pch = vfs::temp_file("clice-test", "pch");
    ZASSERT(pch.operator bool());
    {
        auto preamble = build_preamble(R"(
#include "twice.h"

int main() { return four(); }
)",
                                       *pch);
        ZASSERT(preamble->has_output());
        ZEXPECT(defines_specialization(*preamble, "twice"));
    }
    llvm::sys::fs::remove(*pch);
}

ZEST_CASE(PCHInstantiationFallsBack) {
    add_file("show.h", R"(
#pragma once
template <typename T>
void show(T value) {
    describe(value);
}

namespace app {
struct Point {};
}  // namespace app

inline void demo() {
    show(app::Point{});
}
)");

    auto pch = vfs::temp_file("clice-test", "pch");
    ZASSERT(pch.operator bool());
    {
        // Where the preamble ends, show<app::Point> finds no describe: the
        // PCH leaves it to the parse loading it, which sees main.cpp's.
        auto preamble = build_preamble(R"(
#include "show.h"

namespace app {
void describe(Point) {}
}  // namespace app
)",
                                       *pch);
        ZASSERT(preamble->has_output());
        ZEXPECT(collect_errors(*preamble).empty());
        ZEXPECT(!defines_specialization(*preamble, "show"));
    }
    llvm::sys::fs::remove(*pch);
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
    ZEXPECT(bound1 == bound2);

    auto hash1 = llvm::xxh3_64bits(v1.substr(0, bound1));
    auto hash2 = llvm::xxh3_64bits(v2.substr(0, bound2));
    ZEXPECT(hash1 == hash2);
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
    ZEXPECT(bound1 != bound2);

    auto hash1 = llvm::xxh3_64bits(v1.substr(0, bound1));
    auto hash2 = llvm::xxh3_64bits(v2.substr(0, bound2));
    ZEXPECT(hash1 != hash2);
}

ZEST_CASE(ZeroBoundNoPCH) {
    // No preprocessor directives → bound is 0 → PCH should be skipped.
    llvm::StringRef code = R"cpp(
int main() { return 0; }
)cpp";

    auto bound = compute_preamble_bound(code);
    ZEXPECT(bound == 0u);
}

};  // ZEST_SUITE(PreambleHash)

}  // namespace

}  // namespace clice::testing
