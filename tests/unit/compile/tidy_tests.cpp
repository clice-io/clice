#include "test/temp_dir.h"
#include "test/test.h"
#include "compile/compilation.h"
#include "compile/diagnostic.h"
#include "semantic/content.h"

#include "llvm/Support/Path.h"
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
    // the main file; the finding lands with the header's location.
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
        if(diag.id.source == DiagnosticSource::ClangTidy && diag.fid != unit.main_file()) {
            ASSERT_EQ(diag.id.name, "bugprone-integer-division");
            header_finding = true;
        }
    }
    ASSERT_TRUE(header_finding);
}

/// Two headers with one finding each, parsed with the tidy pass deferred
/// so the test picks the traversal scope itself, as the lint worker does.
struct DeferredTidy {
    llvm::IntrusiveRefCntPtr<TestVFS> vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    std::string main_path = TestVFS::path("main.cpp");
    CompilationUnit unit{nullptr};

    bool compile(llvm::StringRef checks, llvm::StringRef main = "") {
        vfs->add("ratio.h", "inline double ratio(int a, int b) { return a / b; }\n");
        vfs->add("other.h", "inline double other(int a, int b) { return a / b; }\n");
        vfs->add("main.cpp",
                 main.empty() ? "#include \"ratio.h\"\n#include \"other.h\"\n" : main.str());
        CompilationParams params;
        params.kind = CompilationKind::Content;
        params.tidy = tidy::TidyParams{.checks = checks.str(),
                                       .fast_only = false,
                                       .header_filter = ".*",
                                       .whole_tu = true};
        params.defer_tidy = true;
        params.vfs = vfs;
        params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
        unit = clice::compile(params);
        return unit.completed();
    }

    /// The declarations of every content unit in `file`.
    std::vector<clang::Decl*> scope_of(llvm::StringRef file) {
        auto table = ContentTable::compute(unit);
        auto fid = unit.file_id(TestVFS::path(file));
        std::vector<clang::Decl*> scope;
        for(auto& row: table.units) {
            if(row.fid == fid) {
                for(auto* decl: row.decls) {
                    scope.push_back(const_cast<clang::Decl*>(decl));
                }
            }
        }
        return scope;
    }

    /// The files of the check's findings; a suppressed finding stays in
    /// the stream at the Ignored level, a note follows its finding.
    std::vector<std::string> findings(llvm::StringRef check) {
        std::vector<std::string> result;
        for(auto& diag: unit.diagnostics()) {
            if(diag.id.source == DiagnosticSource::ClangTidy && diag.id.name == check &&
               diag.id.level != DiagnosticLevel::Ignored &&
               diag.id.level != DiagnosticLevel::Note) {
                result.push_back(std::string(llvm::sys::path::filename(unit.file_path(diag.fid))));
            }
        }
        return result;
    }
};

TEST_CASE(ScopeSelectsUnits) {
    DeferredTidy tidy;
    ASSERT_TRUE(tidy.compile("-*,bugprone-integer-division"));
    tidy.unit.run_tidy({}, tidy.scope_of("ratio.h"));
    EXPECT_TRUE(tidy.findings("bugprone-integer-division") == std::vector<std::string>{"ratio.h"});
}

TEST_CASE(GroupsOffCheckNothing) {
    DeferredTidy tidy;
    ASSERT_TRUE(tidy.compile("-*,bugprone-integer-division"));
    // No scope means the default traversal, but with both pruned groups
    // off a node-level check never runs.
    tidy.unit.run_tidy({.spelled = false, .nodes = false}, {});
    EXPECT_TRUE(tidy.findings("bugprone-integer-division").empty());
}

TEST_CASE(WholeGroupIgnoresScope) {
    DeferredTidy tidy;
    ASSERT_TRUE(tidy.compile("-*,bugprone-integer-division,misc-unused-using-decls",
                             "#include \"ratio.h\"\nnamespace n { int x; }\nusing n::x;\n"));
    // A TU-level check sees the whole TU whatever the pruned scope is.
    tidy.unit.run_tidy({}, tidy.scope_of("ratio.h"));
    EXPECT_TRUE(tidy.findings("bugprone-integer-division") == std::vector<std::string>{"ratio.h"});
    EXPECT_TRUE(tidy.findings("misc-unused-using-decls") == std::vector<std::string>{"main.cpp"});
}

TEST_CASE(HeaderNolint) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("ratio.h", "inline double ratio(int a, int b) { return a / b; }  // NOLINT\n");
    vfs->add("main.cpp", "#include \"ratio.h\"\n");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.kind = CompilationKind::Content;
    params.tidy = tidy::TidyParams{.checks = "-*,bugprone-integer-division",
                                   .fast_only = false,
                                   .header_filter = ".*",
                                   .whole_tu = true};
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    // A suppressed finding stays in the stream at the Ignored level.
    for(auto& diag: unit.diagnostics()) {
        EXPECT_TRUE(diag.id.source != DiagnosticSource::ClangTidy ||
                    diag.id.level == DiagnosticLevel::Ignored);
    }
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

TEST_CASE(ExtraArgsCommandSplit) {
    // -W warning flags stay on the warning-options path where the Checks
    // gate applies; driver pass-throughs and everything else reach the
    // command halves in order.
    auto split = tidy::command_extra_args({"-DFOO=1", "-Wunused", "-Wp,-DY=2"},
                                          {"-std=c++17", "-Wall", "-fno-exceptions"});
    std::vector<std::string> prepend = {"-std=c++17", "-fno-exceptions"};
    std::vector<std::string> append = {"-DFOO=1", "-Wp,-DY=2"};
    ASSERT_EQ(split.prepend, prepend);
    ASSERT_EQ(split.append, append);

    // A -X<tool> pair filters on its operand's verdict — dropping just
    // the operand would leave the forwarder to eat the next argument.
    auto pairs = tidy::command_extra_args(
        {"-Xclang", "-Wno-unused", "-Xclang", "-fno-exceptions", "-Xclang"},
        {});
    std::vector<std::string> kept = {"-Xclang", "-fno-exceptions", "-Xclang"};
    ASSERT_EQ(pairs.append, kept);
}

};  // TEST_SUITE(ClangTidy)
}  // namespace
}  // namespace clice::testing
