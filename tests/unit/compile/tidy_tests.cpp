module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.diagnostic;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

ZEST_SUITE(ClangTidy) {

using Sources = std::vector<std::pair<std::string, std::string>>;

/// main.cpp of `sources` built for the features under `tidy`, with
/// `flags` added to the command.
CompilationUnit compile_main(const Sources& sources,
                             tidy::TidyParams tidy,
                             llvm::ArrayRef<const char*> flags = {}) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [name, content]: sources) {
        vfs->add(name, content);
    }
    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.kind = CompilationKind::Content;
    params.tidy = std::move(tidy);
    params.vfs = vfs;
    params.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef"};
    llvm::append_range(params.arguments, flags);
    params.arguments.push_back(main_path.c_str());
    return compile(params);
}

/// The checks the unit reports a finding of, sorted.
std::vector<std::string> findings(CompilationUnit& unit) {
    std::vector<std::string> names;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.source == DiagnosticSource::ClangTidy &&
           diag.id.level != DiagnosticLevel::Ignored && diag.id.level != DiagnosticLevel::Note) {
            names.push_back(diag.id.name.str());
        }
    }
    std::ranges::sort(names);
    return names;
}

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
    ZASSERT(expected.empty());
}

ZEST_CASE(PlannedCheckFires) {
    // The frozen plan owns the check set; the matcher walks the top-level
    // declarations only a Content build collects.
    Sources sources = {
        {"main.cpp", "double ratio(int a, int b) { return a / b; }\n"},
    };
    auto unit = compile_main(sources, {.checks = "-*,bugprone-integer-division", .batch = true});
    ZASSERT(unit.completed());
    ZEXPECT(findings(unit) == std::vector<std::string>{"bugprone-integer-division"});
}

ZEST_CASE(BatchTraversesHeaders) {
    Sources sources = {
        {"ratio.h",  "inline double ratio(int a, int b) { return a / b; }\n"},
        {"main.cpp", "#include \"ratio.h\"\nint main() { return 0; }\n"     },
    };
    // The finding lands with the header's location; the header filter is
    // the consumer's to apply.
    auto unit = compile_main(sources, {.checks = "-*,bugprone-integer-division", .batch = true});
    ZASSERT(unit.completed());
    bool header_finding = false;
    for(auto& diag: unit.diagnostics()) {
        header_finding |= diag.id.source == DiagnosticSource::ClangTidy &&
                          diag.id.name == "bugprone-integer-division" &&
                          diag.fid != unit.main_file();
    }
    ZASSERT(header_finding);

    // The editor matches the main file's declarations whatever the
    // configuration reports on.
    auto editor =
        compile_main(sources, {.checks = "-*,bugprone-integer-division", .header_filter = ".*"});
    ZASSERT(editor.completed());
    ZEXPECT(findings(editor).empty());
}

ZEST_CASE(HeaderNolint) {
    Sources sources = {
        {"ratio.h",  "inline double ratio(int a, int b) { return a / b; }  // NOLINT\n"},
        {"main.cpp", "#include \"ratio.h\"\n"                                          },
    };
    auto unit = compile_main(
        sources,
        {.checks = "-*,bugprone-integer-division", .header_filter = ".*", .batch = true});
    ZASSERT(unit.completed());
    // A suppressed finding stays in the stream at the Ignored level.
    bool suppressed = false;
    for(auto& diag: unit.diagnostics()) {
        ZEXPECT((diag.id.source != DiagnosticSource::ClangTidy ||
                 diag.id.level == DiagnosticLevel::Ignored));
        suppressed |= diag.id.source == DiagnosticSource::ClangTidy &&
                      diag.id.name == "bugprone-integer-division" &&
                      diag.id.level == DiagnosticLevel::Ignored;
    }
    ZASSERT(suppressed);
}

ZEST_CASE(EditorDropsUnusableChecks) {
    Sources sources = {
        {"main.cpp",
         "namespace std {\n"
         "template <class T> T&& move(T& t) { return static_cast<T&&>(t); }\n"
         "}\n"
         "struct S { int v; };\n"
         "int use(S s) { S t = std::move(s); return s.v + t.v; }\n"
         "double ratio(int a, int b) { return a / b; }\n"},
    };
    // A configuration cannot enable a check the editor rules out.
    std::string checks = "-*,bugprone-use-after-move,bugprone-integer-division";
    auto editor = compile_main(sources, {.checks = checks});
    ZASSERT(editor.completed());
    ZEXPECT(findings(editor) == std::vector<std::string>{"bugprone-integer-division"});

    auto batch = compile_main(sources, {.checks = checks, .batch = true});
    ZASSERT(batch.completed());
    ZEXPECT(findings(batch) ==
            std::vector<std::string>{"bugprone-integer-division", "bugprone-use-after-move"});
}

ZEST_CASE(EditorSkipsSlowChecks) {
    Sources sources = {
        {"main.cpp", "int one() { int x = 1; return x; }\n"},
    };
    auto editor = compile_main(sources, {.checks = "-*,misc-const-correctness"});
    ZASSERT(editor.completed());
    ZEXPECT(findings(editor).empty());

    auto batch = compile_main(sources, {.checks = "-*,misc-const-correctness", .batch = true});
    ZASSERT(batch.completed());
    ZEXPECT(findings(batch) == std::vector<std::string>{"misc-const-correctness"});
}

ZEST_CASE(CompilerWarningProvenance) {
    Sources sources = {
        {"main.cpp", "int f() { int unused; return 0; }\n"},
    };
    auto warning = [](CompilationUnit& unit) {
        auto it = llvm::find_if(unit.diagnostics(), [](const Diagnostic& diag) {
            return diag.id.level == DiagnosticLevel::Warning;
        });
        return it == unit.diagnostics().end() ? std::optional<DiagnosticID>() : it->id;
    };
    // A checker stays out of a compiler warning in the editor; a batch run
    // takes it as a finding only under its enabled clang-diagnostic name.
    std::string enabled = "-*,bugprone-integer-division,clang-diagnostic-unused-variable";
    auto editor = compile_main(sources, {.checks = enabled}, {"-Wunused-variable"});
    auto id = warning(editor);
    ZASSERT(id);
    ZEXPECT(id->source == DiagnosticSource::Clang);

    auto batch = compile_main(sources, {.checks = enabled, .batch = true}, {"-Wunused-variable"});
    id = warning(batch);
    ZASSERT(id);
    ZEXPECT(id->source == DiagnosticSource::ClangTidy);
    ZEXPECT(id->name == "clang-diagnostic-unused-variable");

    auto other = compile_main(sources,
                              {.checks = "-*,bugprone-integer-division", .batch = true},
                              {"-Wunused-variable"});
    id = warning(other);
    ZASSERT(id);
    ZEXPECT(id->source == DiagnosticSource::Clang);
}

ZEST_CASE(NolintSilencesWarnings) {
    auto unit = compile_main(
        {
            {"main.cpp", "int f() { int unused; return 0; }  // NOLINT\n"}
    },
        {},
        {"-Wunused-variable"});
    ZASSERT(unit.completed());
    bool silenced = false;
    for(auto& diag: unit.diagnostics()) {
        ZEXPECT(diag.id.level != DiagnosticLevel::Warning);
        silenced |= diag.id.level == DiagnosticLevel::Ignored;
    }
    ZASSERT(silenced);
}

ZEST_CASE(OverlappingFixDropped) {
    Sources sources = {
        {"main.cpp", "static int f(int unused) { return 0; }\nint g() { return f(f(1)); }\n"},
    };
    auto unit = compile_main(sources, {.checks = "-*,misc-unused-parameters"});
    ZASSERT(unit.completed());
    auto finding = llvm::find_if(unit.diagnostics(), [](const Diagnostic& diag) {
        return diag.id.name == "misc-unused-parameters";
    });
    ZASSERT(finding != unit.diagnostics().end());
    ZEXPECT(finding->fix.empty());
}

ZEST_CASE(SystemMacroErrorKept) {
    Sources sources = {
        {"sys.h",  "#pragma clang system_header\n#define INIT int *p = 1;\n"},
        {"main.c", "#include \"sys.h\"\nvoid f(void) { INIT }\n"            },
    };
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [name, content]: sources) {
        vfs->add(name, content);
    }
    std::string main_path = TestVFS::path("main.c");
    CompilationParams params;
    params.kind = CompilationKind::Content;
    params.tidy = tidy::TidyParams{};
    params.vfs = vfs;
    params.arguments = {"clang", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    auto unit = compile(params);
    ZASSERT(unit.completed());
    // -Wint-conversion is an error by default; the compiler shows it in a
    // system macro, and clang-tidy keeps out of it.
    ZEXPECT(llvm::any_of(unit.diagnostics(), [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Error;
    }));
}

ZEST_CASE(FixItReplacements) {
    llvm::StringRef content = "int main() { return 0 }\n";
    auto unit = compile_main(
        {
            {"main.cpp", content.str()}
    },
        {});
    ZASSERT(unit.completed());
    auto error = llvm::find_if(unit.diagnostics(), [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Error;
    });
    ZASSERT(error != unit.diagnostics().end());
    auto offset = static_cast<std::uint32_t>(content.find(" }"));
    ZASSERT(error->fix.size() == 1);
    ZEXPECT(error->fix[0].range == LocalSourceRange{offset, offset});
    ZEXPECT(error->fix[0].text == ";");

    // A moved attribute inserts a copy of its own text.
    llvm::StringRef moved = "struct B {};\nstruct D : public [[]] B {};\n";
    auto misplaced = compile_main(
        {
            {"main.cpp", moved.str()}
    },
        {});
    error = llvm::find_if(misplaced.diagnostics(), [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Error;
    });
    ZASSERT(error != misplaced.diagnostics().end());
    auto at = static_cast<std::uint32_t>(moved.find("public"));
    auto attribute = static_cast<std::uint32_t>(moved.find("[[]]"));
    ZASSERT(error->fix.size() == 2);
    ZEXPECT(error->fix[0].range == LocalSourceRange{at, at});
    ZEXPECT(error->fix[0].text == "[[]]");
    ZEXPECT(error->fix[1].range == LocalSourceRange{attribute, attribute + 4});
    ZEXPECT(error->fix[1].text.empty());
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
    auto resolution = tidy::resolve_tidy_params(tmp.path("sub/a.cpp"));
    auto& params = resolution.params;
    ZASSERT(params.checks.contains("bugprone-*"));
    ZASSERT(params.checks.contains("modernize-*"));
    // What the resolution read, nearest first; the walk stops at a file
    // that inherits nothing.
    ZASSERT(resolution.files.size() == 2);
    ZEXPECT(resolution.files[0].path == tmp.path("sub/.clang-tidy"));
    ZEXPECT(resolution.files[0].hash != 0);
    ZEXPECT(resolution.files[1].path == tmp.path(".clang-tidy"));
    ZEXPECT(!resolution.files[1].absent);

    auto parent = tidy::resolve_tidy_params(tmp.path("a.cpp")).params;
    ZASSERT(parent.checks.contains("bugprone-*"));
    ZASSERT(!parent.checks.contains("modernize-*"));
    ZASSERT(parent.warnings_as_errors == "bugprone-*");
    ZASSERT(parent.header_filter == ".*");
    ZASSERT(parent.exclude_header_filter == "third_party/.*");
}

ZEST_CASE(ResolveWithoutConfig) {
    TempDir tmp;
    tmp.touch("a.cpp");
    auto resolution = tidy::resolve_tidy_params(tmp.path("a.cpp"));
    ZASSERT(resolution.params.checks.empty());
    // Every directory up to the root looked in, none holding one.
    ZASSERT(!resolution.files.empty());
    ZEXPECT(resolution.files[0].path == tmp.path(".clang-tidy"));
    ZEXPECT(llvm::all_of(resolution.files, [](const DepFile& file) { return file.absent; }));
}

ZEST_CASE(ExtraArgsCommandSplit) {
    // -W warning flags stay on the warning-options path where the Checks
    // gate applies; driver pass-throughs and everything else reach the
    // command halves in order.
    auto split = tidy::command_extra_args({"-DFOO=1", "-Wunused", "-Wp,-DY=2"},
                                          {"-std=c++17", "-Wall", "-fno-exceptions"});
    std::vector<std::string> prepend = {"-std=c++17", "-fno-exceptions"};
    std::vector<std::string> append = {"-DFOO=1", "-Wp,-DY=2"};
    ZASSERT(split.prepend == prepend);
    ZASSERT(split.append == append);

    // A -X<tool> pair filters on its operand's verdict — dropping just
    // the operand would leave the forwarder to eat the next argument.
    auto pairs = tidy::command_extra_args(
        {"-Xclang", "-Wno-unused", "-Xclang", "-fno-exceptions", "-Xclang"},
        {});
    std::vector<std::string> kept = {"-Xclang", "-fno-exceptions", "-Xclang"};
    ZASSERT(pairs.append == kept);
}

};  // ZEST_SUITE(ClangTidy)
}  // namespace
}  // namespace clice::testing
