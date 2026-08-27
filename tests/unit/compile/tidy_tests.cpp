#include "test/temp_dir.h"
#include "test/test.h"
#include "compile/compilation.h"
#include "compile/diagnostic.h"

#include "clang-tidy/ClangTidyModuleRegistry.h"

namespace clice::testing {
namespace {

TEST_SUITE(ClangTidy) {

TEST_CASE(ModulesLinked) {
    llvm::StringSet<> expected = {
        "abseil-module",      "altera-module",
        "android-module",     "boost-module",
        "bugprone-module",    "cert-module",
        "concurrency-module", "cppcoreguidelines-module",
        "darwin-module",      "fuchsia-module",
        "google-module",      "hicpp-module",
        "linux-module",       "llvm-module",
        "llvmlibc-module",    "misc-module",
        "modernize-module",   "mpi-module",
        "objc-module",        "openmp-module",
        "performance-module", "portability-module",
        "readability-module", "zircon-module",
    };

    for(auto& entry: clang::tidy::ClangTidyModuleRegistry::entries()) {
        expected.erase(entry.getName());
    }
    // Debug links shared libs (all 24 modules); Release uses static libs
    // where --gc-sections strips mpi-module (CLANG_TIDY_ENABLE_STATIC_ANALYZER=0).
    expected.erase("mpi-module");
    ASSERT_TRUE(expected.empty());
}

TEST_CASE(Tidy) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("main.cpp", "int main() { return 0 }");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.tidy = tidy::TidyParams{};
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_FALSE(unit.diagnostics().empty());
}

TEST_CASE(PlannedCheckFires) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("main.cpp", "double ratio(int a, int b) { return a / b; }\n");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    // The frozen plan owns the check set; the matcher walks the top-level
    // declarations only a Content build collects.
    params.kind = CompilationKind::Content;
    params.tidy = tidy::TidyParams{.checks = "-*,bugprone-integer-division", .fast_only = false};
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());

    bool fired = false;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.source == DiagnosticSource::ClangTidy) {
            ASSERT_EQ(diag.id.name, "bugprone-integer-division");
            fired = true;
        }
    }
    ASSERT_TRUE(fired);
}

TEST_CASE(HeaderFilterTraversesHeaders) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("ratio.h", "inline double ratio(int a, int b) { return a / b; }\n");
    vfs->add("main.cpp", "#include \"ratio.h\"\nint main() { return 0; }\n");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    // A header-reporting configuration widens the matcher traversal past
    // the interested file; the finding lands with the header's location.
    params.kind = CompilationKind::Content;
    params.tidy = tidy::TidyParams{.checks = "-*,bugprone-integer-division",
                                   .fast_only = false,
                                   .header_filter = ".*"};
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());

    bool header_finding = false;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.source == DiagnosticSource::ClangTidy && diag.fid != unit.interested_file()) {
            ASSERT_EQ(diag.id.name, "bugprone-integer-division");
            header_finding = true;
        }
    }
    ASSERT_TRUE(header_finding);
}

TEST_CASE(ResolveConfigChain) {
    TempDir tmp;
    tmp.touch(".clang-tidy",
              "Checks: '-*,bugprone-*'\n"
              "WarningsAsErrors: 'bugprone-*'\n"
              "HeaderFilterRegex: '.*'\n"
              "ExcludeHeaderFilterRegex: 'third_party/.*'\n");
    tmp.touch("sub/.clang-tidy", "InheritParentConfig: true\nChecks: 'modernize-*'\n");
    tmp.touch("sub/a.cpp");

    // Nested configs merge with clang-tidy's own semantics: the child
    // appends to the inherited parent list.
    auto params = tidy::resolve_tidy_params(tmp.path("sub/a.cpp"));
    ASSERT_TRUE(params.checks.contains("bugprone-*"));
    ASSERT_TRUE(params.checks.contains("modernize-*"));

    auto parent = tidy::resolve_tidy_params(tmp.path("a.cpp"));
    ASSERT_TRUE(parent.checks.contains("bugprone-*"));
    ASSERT_FALSE(parent.checks.contains("modernize-*"));
    ASSERT_EQ(parent.warnings_as_errors, "bugprone-*");
    ASSERT_EQ(parent.header_filter, ".*");
    ASSERT_EQ(parent.exclude_header_filter, "third_party/.*");
}

TEST_CASE(ResolveWithoutConfig) {
    TempDir tmp;
    tmp.touch("a.cpp");
    ASSERT_TRUE(tidy::resolve_tidy_params(tmp.path("a.cpp")).checks.empty());
}

TEST_CASE(ExtraArgsReachCommand) {
    tidy::TidyParams params;
    params.extra_args = {"-DFOO=1", "-Wunused"};
    params.extra_args_before = {"-std=c++17", "-Wall", "-fno-exceptions"};

    // Before-args land after the binary name in order, extra args append
    // at the end; -W flags stay on the warning-options path.
    std::vector<const char*> args = {"clang++", "main.cpp"};
    auto owned = tidy::apply_compile_args(params, args);
    std::vector<std::string> applied(args.begin(), args.end());
    std::vector<std::string> expected = {"clang++",
                                         "-std=c++17",
                                         "-fno-exceptions",
                                         "main.cpp",
                                         "-DFOO=1"};
    ASSERT_EQ(applied, expected);

    // A command without a binary name keeps the before-args in front.
    std::vector<const char*> bare = {"-x", "c++"};
    auto bare_owned = tidy::apply_compile_args(params, bare);
    ASSERT_EQ(llvm::StringRef(bare[0]), "-std=c++17");

    // Driver pass-throughs are not warning flags: on a driver command
    // they apply verbatim.
    tidy::TidyParams pass;
    pass.extra_args_before = {"-DX=1", "-Wp,-DY=2,-DZ", "-Wl,-s", "-Wall"};
    std::vector<const char*> drv = {"clang++", "main.cpp"};
    auto drv_owned = tidy::apply_compile_args(pass, drv);
    std::vector<std::string> drv_got(drv.begin(), drv.end());
    std::vector<std::string> drv_want = {"clang++", "-DX=1", "-Wp,-DY=2,-DZ", "-Wl,-s", "main.cpp"};
    ASSERT_EQ(drv_got, drv_want);

    // A resolved cc1 command keeps -cc1 in mode position; with no driver
    // to unwrap pass-throughs, -Wp, splits into its preprocessor
    // arguments and -Wl,/-Wa, drop.
    std::vector<const char*> cc1 = {"clang", "-cc1", "main.cpp"};
    auto cc1_owned = tidy::apply_compile_args(pass, cc1);
    std::vector<std::string> got(cc1.begin(), cc1.end());
    std::vector<std::string> want = {"clang", "-cc1", "-DX=1", "-DY=2", "-DZ", "main.cpp"};
    ASSERT_EQ(got, want);
}

};  // TEST_SUITE(ClangTidy)
}  // namespace
}  // namespace clice::testing
