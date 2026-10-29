module clice;

import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(ClangTidy) {

using Sources = std::vector<std::pair<std::string, std::string>>;

/// main.cpp of `sources` compiled as `kind` under `tidy`, with `flags`
/// added to the command.
std::unique_ptr<CompilationUnit> compile_main(const Sources& sources,
                                              tidy::TidyParams tidy,
                                              llvm::ArrayRef<const char*> flags = {},
                                              CompilationKind kind = CompilationKind::Content) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [name, content]: sources) {
        vfs->add(name, content);
    }
    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.kind = kind;
    params.tidy = std::move(tidy);
    params.input.vfs = vfs;
    params.input.arguments = {"clang++", "-ffreestanding", "-Xclang", "-undef"};
    params.input.arguments.append_range(flags);
    params.input.arguments.push_back(main_path);
    return compile(std::move(params));
}

/// The checks the unit reports a finding of, sorted: a batch run's
/// findings, or the editor's clang-tidy diagnostics.
std::vector<std::string> findings(CompilationUnit& unit) {
    std::vector<std::string> names;
    if(unit.kind() == CompilationKind::Whole) {
        for(auto& finding: unit.take_tidy_findings()) {
            names.push_back(finding.DiagnosticName);
        }
    } else {
        for(auto& diag: unit.diagnostics()) {
            if(diag.id.source == DiagnosticSource::ClangTidy) {
                names.push_back(diag.id.name.str());
            }
        }
    }
    ranges::sort(names);
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
    // The frozen plan owns the check set.
    Sources sources = {
        {"main.cpp", "double ratio(int a, int b) { return a / b; }\n"},
    };
    auto unit = compile_main(sources,
                             {.checks = "-*,bugprone-integer-division"},
                             {},
                             CompilationKind::Whole);
    ZASSERT(unit->completed());
    ZEXPECT(findings(*unit) == std::vector<std::string>{"bugprone-integer-division"});
}

ZEST_CASE(BatchTraversesHeaders) {
    Sources sources = {
        {"ratio.h",  "inline double ratio(int a, int b) { return a / b; }\n"},
        {"main.cpp", "#include \"ratio.h\"\nint main() { return 0; }\n"     },
    };
    auto unit = compile_main(sources,
                             {.checks = "-*,bugprone-integer-division", .header_filter = ".*"},
                             {},
                             CompilationKind::Whole);
    ZASSERT(unit->completed());
    auto reported = unit->take_tidy_findings();
    ZASSERT(reported.size() == 1);
    ZEXPECT(reported[0].DiagnosticName == "bugprone-integer-division");
    ZEXPECT(llvm::StringRef(reported[0].Message.FilePath).ends_with("ratio.h"));

    // No header filter, no header findings.
    auto filtered = compile_main(sources,
                                 {.checks = "-*,bugprone-integer-division"},
                                 {},
                                 CompilationKind::Whole);
    ZASSERT(filtered->completed());
    ZEXPECT(findings(*filtered).empty());

    // The editor matches the focus file's declarations whatever the
    // configuration reports on.
    auto editor =
        compile_main(sources, {.checks = "-*,bugprone-integer-division", .header_filter = ".*"});
    ZASSERT(editor->completed());
    ZEXPECT(findings(*editor).empty());
}

ZEST_CASE(HeaderNolint) {
    auto compile_header = [&](llvm::StringRef header) {
        Sources sources = {
            {"ratio.h",  header.str()            },
            {"main.cpp", "#include \"ratio.h\"\n"},
        };
        return compile_main(sources,
                            {.checks = "-*,bugprone-integer-division", .header_filter = ".*"},
                            {},
                            CompilationKind::Whole);
    };
    auto reported = compile_header("inline double ratio(int a, int b) { return a / b; }\n");
    ZASSERT(reported->completed());
    ZEXPECT(findings(*reported) == std::vector<std::string>{"bugprone-integer-division"});

    auto suppressed =
        compile_header("inline double ratio(int a, int b) { return a / b; }  // NOLINT\n");
    ZASSERT(suppressed->completed());
    ZEXPECT(findings(*suppressed).empty());
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
    ZASSERT(editor->completed());
    ZEXPECT(findings(*editor) == std::vector<std::string>{"bugprone-integer-division"});

    auto batch = compile_main(sources, {.checks = checks}, {}, CompilationKind::Whole);
    ZASSERT(batch->completed());
    ZEXPECT(findings(*batch) ==
            std::vector<std::string>{"bugprone-integer-division", "bugprone-use-after-move"});
}

ZEST_CASE(StdMoveStaysOff) {
    Sources sources = {
        {"main.cpp",
         "struct S {\n"
         "    S& operator=(const S&);\n"
         "    S& operator=(S&&);\n"
         "};\n"
         "void assign(S& out) { S s; out = s; }\n"},
    };
    std::string checks = "-*,performance-use-std-move";
    auto editor = compile_main(sources, {.checks = checks});
    ZASSERT(editor->completed());
    ZEXPECT(findings(*editor).empty());

    auto batch = compile_main(sources, {.checks = checks}, {}, CompilationKind::Whole);
    ZASSERT(batch->completed());
    ZEXPECT(findings(*batch).empty());
}

ZEST_CASE(EditorSkipsSlowChecks) {
    Sources sources = {
        {"main.cpp", "int one() { int x = 1; return x; }\n"},
    };
    auto editor = compile_main(sources, {.checks = "-*,misc-const-correctness"});
    ZASSERT(editor->completed());
    ZEXPECT(findings(*editor).empty());

    auto batch =
        compile_main(sources, {.checks = "-*,misc-const-correctness"}, {}, CompilationKind::Whole);
    ZASSERT(batch->completed());
    ZEXPECT(findings(*batch) == std::vector<std::string>{"misc-const-correctness"});
}

ZEST_CASE(CompilerWarningProvenance) {
    Sources sources = {
        {"main.cpp", "int f() { int unused; return 0; }\n"},
    };
    auto warning = [](CompilationUnit& unit) {
        auto diagnostics = unit.diagnostics();
        auto it = ranges::find_if(diagnostics, [](const Diagnostic& diag) {
            return diag.id.level == DiagnosticLevel::Warning;
        });
        return it == diagnostics.end() ? std::optional<DiagnosticID>() : it->id;
    };
    // A checker stays out of a compiler warning in the editor; a batch run
    // takes it as a finding only under its enabled clang-diagnostic name.
    std::string enabled = "-*,bugprone-integer-division,clang-diagnostic-unused-variable";
    auto editor = compile_main(sources, {.checks = enabled}, {"-Wunused-variable"});
    auto id = warning(*editor);
    ZASSERT(id);
    ZEXPECT(id->source == DiagnosticSource::Clang);

    auto batch =
        compile_main(sources, {.checks = enabled}, {"-Wunused-variable"}, CompilationKind::Whole);
    ZASSERT(batch->completed());
    ZEXPECT(findings(*batch) == std::vector<std::string>{"clang-diagnostic-unused-variable"});

    auto other = compile_main(sources,
                              {.checks = "-*,bugprone-integer-division"},
                              {"-Wunused-variable"},
                              CompilationKind::Whole);
    ZASSERT(other->completed());
    ZEXPECT(findings(*other).empty());
}

ZEST_CASE(NolintSilencesWarnings) {
    auto warnings = [](CompilationUnit& unit) {
        return ranges::count_if(unit.diagnostics(), [](const Diagnostic& diag) {
            return diag.id.level == DiagnosticLevel::Warning;
        });
    };
    auto reported = compile_main(
        {
            {"main.cpp", "int f() { int unused; return 0; }\n"}
    },
        {},
        {"-Wunused-variable"});
    ZASSERT(reported->completed());
    ZEXPECT(warnings(*reported) == 1);

    auto silenced = compile_main(
        {
            {"main.cpp", "int f() { int unused; return 0; }  // NOLINT\n"}
    },
        {},
        {"-Wunused-variable"});
    ZASSERT(silenced->completed());
    ZEXPECT(warnings(*silenced) == 0);
}

ZEST_CASE(OverlappingFixDropped) {
    Sources sources = {
        {"main.cpp", "static int f(int unused) { return 0; }\nint g() { return f(f(1)); }\n"},
    };
    auto unit = compile_main(sources, {.checks = "-*,misc-unused-parameters"});
    ZASSERT(unit->completed());
    auto diagnostics = unit->diagnostics();
    auto finding = ranges::find_if(diagnostics, [](const Diagnostic& diag) {
        return diag.id.name == "misc-unused-parameters";
    });
    ZASSERT(finding != diagnostics.end());
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
    params.input.vfs = vfs;
    params.input.arguments = {"clang", "-ffreestanding", "-Xclang", "-undef", main_path};
    auto unit = compile(std::move(params));
    ZASSERT(unit->completed());
    // -Wint-conversion is an error by default; the compiler shows it in a
    // system macro, and clang-tidy keeps out of it.
    ZEXPECT(ranges::any_of(unit->diagnostics(), [](const Diagnostic& diag) {
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
    ZASSERT(unit->completed());
    auto diagnostics = unit->diagnostics();
    auto error = ranges::find_if(diagnostics, [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Error;
    });
    ZASSERT(error != diagnostics.end());
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
    auto misplaced_diagnostics = misplaced->diagnostics();
    error = ranges::find_if(misplaced_diagnostics, [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Error;
    });
    ZASSERT(error != misplaced_diagnostics.end());
    auto at = static_cast<std::uint32_t>(moved.find("public"));
    auto attribute = static_cast<std::uint32_t>(moved.find("[[]]"));
    ZASSERT(error->fix.size() == 2);
    ZEXPECT(error->fix[0].range == LocalSourceRange{at, at});
    ZEXPECT(error->fix[0].text == "[[]]");
    ZEXPECT(error->fix[1].range == LocalSourceRange{attribute, attribute + 4});
    ZEXPECT(error->fix[1].text.empty());
}

ZEST_CASE(MacroArgumentFixes) {
    llvm::StringRef content = R"(#define ASSIGN_IF(x) if (x = 1) {}
#define CHECK(x) (x)
int f(int value, int total) {
    ASSIGN_IF(value);
    return CHECK(totl);
}
)";
    auto unit = compile_main(
        {
            {"main.cpp", content.str()}
    },
        {});
    ZASSERT(unit->completed());
    // Clang drops the `)` it cannot place after `1` in the definition: the
    // `(` left in the argument is no fix.
    auto diagnostics = unit->diagnostics();
    auto assignment = ranges::find_if(diagnostics, [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Warning;
    });
    ZASSERT(assignment != diagnostics.end());
    auto parentheses = ranges::find_if(assignment->notes, [](const Diagnostic& note) {
        return llvm::StringRef(note.message).starts_with("place parentheses");
    });
    ZASSERT(parentheses != assignment->notes.end());
    ZEXPECT(parentheses->fix.empty());

    auto typo = ranges::find_if(diagnostics, [](const Diagnostic& diag) {
        return diag.id.level == DiagnosticLevel::Error;
    });
    ZASSERT(typo != diagnostics.end());
    auto offset = static_cast<std::uint32_t>(content.find("totl"));
    ZASSERT(typo->fix.size() == 1);
    ZEXPECT(typo->fix[0].range == LocalSourceRange{offset, offset + 4});
    ZEXPECT(typo->fix[0].text == "total");
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
    ZEXPECT(ranges::all_of(resolution.files, [](const DepFile& file) { return file.absent; }));
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

    // A -X<tool> pair filters on its operand's decision — dropping just
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
