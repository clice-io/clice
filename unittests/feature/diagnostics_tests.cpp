module clice;

import :command.table;
import :compile.diagnostic;
import :feature.diagnostics;
import :unittests.utils;

namespace clice::testing {

namespace {

feature::FileDiagnostic error_at(std::uint32_t begin, std::string message, std::uint32_t id = 0) {
    return {
        .range = {begin, begin + 1},
        .level = DiagnosticLevel::Error,
        .source = DiagnosticSource::Clang,
        .id = id,
        .message = std::move(message),
    };
}

ZEST_SUITE(diagnostics) {

ZEST_CASE(InferredCommandWarns) {
    /// A guessed command explains an include it did not find, at the top.
    feature::CompileDiagnostics compiled{
        .source = CommandSource::Inferred,
        .own = {error_at(10, "'a.h' file not found", clang::diag::err_pp_file_not_found)},
    };
    auto diagnostics = feature::shown_diagnostics(compiled, {});
    ZASSERT(diagnostics.size() == 2u);
    ZEXPECT(diagnostics[0].code == "inferred-compile-command");
    ZEXPECT(llvm::StringRef(diagnostics[0].message).contains("borrowed from a nearby"));
    ZEXPECT(!diagnostics[0].link.empty());

    /// The database's own command, or a missing include it cannot explain,
    /// gets none.
    compiled.source = CommandSource::CDBExact;
    ZEXPECT(feature::shown_diagnostics(compiled, {}).size() == 1u);
    compiled.source = CommandSource::Fallback;
    compiled.own = {error_at(10, "use of undeclared identifier 'x'")};
    ZEXPECT(feature::shown_diagnostics(compiled, {}).size() == 1u);
}

ZEST_CASE(UnmatchedContextWarns) {
    /// A header whose includer context could not be rebuilt says so at its
    /// top, naming the host.
    auto diagnostics = feature::shown_diagnostics(
        {
            .source = CommandSource::Inferred,
            .unmatched_host = "src/a.cpp",
        },
        {});
    ZASSERT(diagnostics.size() == 1u);
    ZEXPECT(diagnostics[0].code == "unmatched-includer-context");
    ZEXPECT(diagnostics[0].range.begin == 0u);
    ZEXPECT(feature::shown_diagnostics({.source = CommandSource::IncludeGraph}, {}).empty());
}

ZEST_CASE(QueryFailureWarns) {
    /// A compile whose compiler could not be queried says so at its top,
    /// with the query's error.
    auto diagnostics = feature::shown_diagnostics(
        {
            .source = CommandSource::CDBExact,
            .query_failure = "Driver riscv32-wch-elf-gcc not found or not executable",
        },
        {});
    ZASSERT(diagnostics.size() == 1u);
    ZEXPECT(diagnostics[0].code == "toolchain-query-failed");
    ZEXPECT(llvm::StringRef(diagnostics[0].message).contains("riscv32-wch-elf-gcc not found"));
    ZEXPECT(diagnostics[0].range.begin == 0u);

    /// A project that knows its compiler is not here hides it by its code.
    feature::DiagnosticsOptions options;
    options.suppress = {"toolchain-query-failed"};
    ZEXPECT(feature::shown_diagnostics({.query_failure = "Driver gcc not found or not executable"},
                                       options)
                .empty());
}

ZEST_CASE(RelocatedShownOnce) {
    /// Of the diagnostics moved to one #include, the first error and the
    /// first warning show; the skipped ones follow the compile's own.
    auto moved = [](std::string message, DiagnosticLevel level) {
        auto diagnostic = error_at(4, std::move(message));
        diagnostic.level = level;
        diagnostic.origin = feature::DiagnosticOrigin::IncludedFile;
        return diagnostic;
    };
    auto diagnostics = feature::shown_diagnostics(
        {
            .own = {moved("first error", DiagnosticLevel::Error),
                    moved("second error", DiagnosticLevel::Error),
                    moved("a warning", DiagnosticLevel::Warning)},
            .skipped = {error_at(20, "from the full compile")},
    },
        {});
    ZASSERT(diagnostics.size() == 3u);
    ZEXPECT(diagnostics[0].message == "first error");
    ZEXPECT(diagnostics[1].message == "a warning");
    ZEXPECT(diagnostics[2].message == "from the full compile");
}

ZEST_CASE(PreambleFirstOnce) {
    /// The PCH's preamble diagnostics come first; one the parse raises
    /// again, as the command line's are, shows once.
    auto diagnostics =
        feature::with_preamble({error_at(0, "command line"), error_at(30, "body")},
                               {error_at(5, "preamble"), error_at(0, "command line")});
    ZASSERT(diagnostics.size() == 3u);
    ZEXPECT(diagnostics[0].message == "preamble");
    ZEXPECT(diagnostics[1].message == "command line");
    ZEXPECT(diagnostics[2].message == "body");
}

ZEST_CASE(SkippedConfirmed) {
    /// A compile skipping header bodies holds its unused-declaration
    /// warnings back; the full compile's report confirms one it also
    /// raised, and adds what the compile did not raise itself.
    std::vector<feature::FileDiagnostic> raised{
        error_at(3, "unused function 'f'", clang::diag::warn_unused_function),
        error_at(9, "own error"),
    };
    auto unconfirmed = feature::take_unconfirmed(raised);
    ZASSERT(unconfirmed.size() == 1u);
    ZASSERT(raised.size() == 1u);

    feature::CompileDiagnostics compiled{.own = raised, .unconfirmed = unconfirmed};
    auto shown = feature::confirmed_skipped(
        {
            error_at(3, "unused function 'f'", clang::diag::warn_unused_function),
            error_at(5, "unused function 'g'", clang::diag::warn_unused_function),
            error_at(9, "own error"),
            error_at(12, "in an instantiation"),
        },
        compiled);
    ZASSERT(shown.size() == 2u);
    ZEXPECT(shown[0].message == "unused function 'f'");
    ZEXPECT(shown[1].message == "in an instantiation");
}

ZEST_CASE(FailedImportExplained) {
    /// An import of a module whose build failed says why, not "not found".
    std::vector<feature::FileDiagnostic> diagnostics{
        error_at(0, "module 'app' not found", clang::diag::err_module_not_found),
        error_at(8, "module 'loop' not found", clang::diag::err_module_not_found),
    };
    feature::explain_failed_imports(
        diagnostics,
        {
            {.chain = {"app", "core"},  .error = "core.cppm:1:1: error: boom"},
            {.chain = {"loop", "back"}, .cycle = 0                           },
    });
    ZEXPECT(
        diagnostics[0].message ==
        "could not build module 'app': could not build module 'core': core.cppm:1:1: error: boom");
    ZEXPECT(diagnostics[0].code == "err_module_not_built");
    ZEXPECT(diagnostics[1].message == "cyclic dependency in module 'loop': loop -> back -> loop");
    ZEXPECT(diagnostics[1].code == "err_module_cycle");
}

ZEST_CASE(AmbiguousModuleWarns) {
    /// The warning sits on the import's line when the file writes it, at
    /// the top otherwise.
    llvm::StringRef text = "module;\nimport shapes;\nint x;\n";
    feature::ModuleChoice choice{
        .module = "shapes",
        .chosen = "src/shapes.cppm",
        .others = {"old/shapes.cppm", "vendor/shapes.cppm"},
        .offset = 8,
    };
    auto warning = feature::ambiguous_module(choice, text);
    ZEXPECT(warning.code == "ambiguous-module");
    ZEXPECT(warning.message ==
            "module 'shapes' is declared by more than one file; this file imports "
            "src/shapes.cppm, the nearest, not old/shapes.cppm, vendor/shapes.cppm");
    ZEXPECT(warning.range.begin == 8u);
    ZEXPECT(warning.range.end == 22u);

    choice.offset = static_cast<std::uint32_t>(text.size());
    ZEXPECT(feature::ambiguous_module(choice, text).range.begin == 0u);
}

ZEST_CASE(OwnWarnings) {
    auto tidy = feature::invalid_tidy_config("unknown key 'Chekcs'");
    ZEXPECT(tidy.code == "invalid-clang-tidy-config");
    ZEXPECT(llvm::StringRef(tidy.message).ends_with("unknown key 'Chekcs'"));
    ZEXPECT(tidy.range.begin == 0u);

    auto index = feature::index_unavailable("the index is too large");
    ZEXPECT(llvm::StringRef(index.message).starts_with("the index is too large; features"));
    ZEXPECT(index.level == DiagnosticLevel::Warning);
}

};  // ZEST_SUITE(diagnostics)

}  // namespace

}  // namespace clice::testing
