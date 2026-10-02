#include "test/temp_dir.h"
#include "test/test.h"
#include "compile/compilation.h"
#include "compile/diagnostic.h"

#include "clang-tidy/ClangTidyModule.h"

namespace clice::testing {
namespace {

ZEST_SUITE(ClangTidy) {

ZEST_CASE(ModulesLinked) {
    llvm::StringSet<> expected = {
        "abseil-module",      "altera-module",   "android-module",     "boost-module",
        "bugprone-module",    "cert-module",     "concurrency-module", "cppcoreguidelines-module",
        "darwin-module",      "fuchsia-module",  "google-module",      "linux-module",
        "llvm-module",        "llvmlibc-module", "misc-module",        "modernize-module",
        "objc-module",        "openmp-module",   "performance-module", "portability-module",
        "readability-module", "zircon-module",
    };

    for(auto& entry: clang::tidy::ClangTidyModuleRegistry::entries()) {
        expected.erase(entry.getName());
    }
    ASSERT(expected.empty());

}  // namespace

ZEST_CASE(Tidy) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("main.cpp", "int main() { return 0 }");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.tidy = tidy::TidyParams{};
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ASSERT(unit.completed());
    ASSERT(!unit.diagnostics().empty());
}

ZEST_CASE(PlannedCheckFires) {
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
    ASSERT(unit.completed());

    bool fired = false;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.source == DiagnosticSource::ClangTidy) {
            ASSERT(diag.id.name == "bugprone-integer-division");
            fired = true;
        }
    }
    ASSERT(fired);
}

ZEST_CASE(HeaderFilterTraversesHeaders) {
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
    ASSERT(unit.completed());

    bool header_finding = false;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.source == DiagnosticSource::ClangTidy && diag.fid != unit.main_file()) {
            ASSERT(diag.id.name == "bugprone-integer-division");
            header_finding = true;
        }
    }
    ASSERT(header_finding);
}

ZEST_CASE(HeaderNolint) {
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
    ASSERT(unit.completed());
    // A suppressed finding stays in the stream at the Ignored level.
    bool suppressed = false;
    for(auto& diag: unit.diagnostics()) {
        EXPECT((diag.id.source != DiagnosticSource::ClangTidy ||
                diag.id.level == DiagnosticLevel::Ignored));
        suppressed |= diag.id.source == DiagnosticSource::ClangTidy &&
                      diag.id.name == "bugprone-integer-division" &&
                      diag.id.level == DiagnosticLevel::Ignored;
    }
    ASSERT(suppressed);
}

ZEST_CASE(ResolveConfigChain) {
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
    ASSERT(params.checks.contains("bugprone-*"));
    ASSERT(params.checks.contains("modernize-*"));

    auto parent = tidy::resolve_tidy_params(tmp.path("a.cpp"));
    ASSERT(parent.checks.contains("bugprone-*"));
    ASSERT(!parent.checks.contains("modernize-*"));
    ASSERT(parent.warnings_as_errors == "bugprone-*");
    ASSERT(parent.header_filter == ".*");
    ASSERT(parent.exclude_header_filter == "third_party/.*");
}

ZEST_CASE(ResolveWithoutConfig) {
    TempDir tmp;
    tmp.touch("a.cpp");
    ASSERT(tidy::resolve_tidy_params(tmp.path("a.cpp")).checks.empty());
}

ZEST_CASE(ExtraArgsCommandSplit) {
    // -W warning flags stay on the warning-options path where the Checks
    // gate applies; driver pass-throughs and everything else reach the
    // command halves in order.
    auto split = tidy::command_extra_args({"-DFOO=1", "-Wunused", "-Wp,-DY=2"},
                                          {"-std=c++17", "-Wall", "-fno-exceptions"});
    std::vector<std::string> prepend = {"-std=c++17", "-fno-exceptions"};
    std::vector<std::string> append = {"-DFOO=1", "-Wp,-DY=2"};
    ASSERT(split.prepend == prepend);
    ASSERT(split.append == append);

    // A -X<tool> pair filters on its operand's verdict — dropping just
    // the operand would leave the forwarder to eat the next argument.
    auto pairs = tidy::command_extra_args(
        {"-Xclang", "-Wno-unused", "-Xclang", "-fno-exceptions", "-Xclang"},
        {});
    std::vector<std::string> kept = {"-Xclang", "-fno-exceptions", "-Xclang"};
    ASSERT(pairs.append == kept);
}

};  // ZEST_SUITE(ClangTidy)
}  // namespace
}  // namespace clice::testing
