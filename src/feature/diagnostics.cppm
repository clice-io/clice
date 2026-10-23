module;

#include "support/prelude.h"

module clice:feature.diagnostics;

import :compile.compilation;
import :compile.diagnostic;
import :syntax.position;

namespace clice::feature {

/// Corresponds to the `[diagnostics]` section in clice.toml.
struct DiagnosticsOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Run clang-tidy on open files and offer its fixes, with "
                         "the checks the nearest `.clang-tidy` configures (a "
                         "small default set without one). Checks too slow for "
                         "an editor, and those unreliable on code being edited, "
                         "are left out; files `clice lint` does not check get "
                         "none. `NOLINT` comments also silence compiler warnings "
                         "only while this runs.")
    <bool> clang_tidy = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Diagnostics open files do not show, by their code: a "
                         "compiler warning's option (`-Wunused-variable`), "
                         "clang's name for any other diagnostic "
                         "(`err_undeclared_var_use`), or a clang-tidy check. "
                         "For code clang gets wrong: everything else works on "
                         "the file as before, and `clice lint` still reports "
                         "them.")
    <std::vector<std::string>> suppress;
};

/// Where a diagnostic shown on a file was raised, when not in the file's
/// own text.
enum class DiagnosticOrigin : std::uint8_t {
    /// The file's text, or the command line.
    File,

    /// A template the file instantiates: shown where the file asks for the
    /// instantiation.
    Template,

    /// A header the file includes: shown on its #include.
    IncludedFile,

    /// A module unit the file imports, directly or not: shown on its
    /// import.
    ImportedModule,

    /// The includer's code a header's borrowed context compiles around it.
    IncluderContext,
};

/// A place a diagnostic's note points at.
struct DiagnosticNote {
    std::string path;
    protocol::Range range;
    std::string message;
};

/// A diagnostic as shown on one file, in the file's positions: the
/// compiler's and clang-tidy's of its compile, and clice's own about the
/// file. Each client gets it in the shape it takes (to_protocol).
struct FileDiagnostic {
    protocol::Range range;
    DiagnosticLevel level = DiagnosticLevel::Warning;
    DiagnosticSource source = DiagnosticSource::Clice;

    /// clang's ID of a compiler diagnostic; 0 for the others.
    std::uint32_t id = 0;

    /// See DiagnosticID::diagnostic_code.
    std::string code;

    /// The page documenting the code; empty for none.
    std::string link;

    /// The message as reported, without where it came from.
    std::string message;

    DiagnosticOrigin origin = DiagnosticOrigin::File;

    std::vector<DiagnosticNote> notes;
};

/// The diagnostics a compile shows on its focus file.
auto diagnostics(CompilationUnit& unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<FileDiagnostic>;

/// Of the diagnostics of a compile parsing every body, those a compile
/// skipping the bodies of the headers (CompilationParams::skip_bodies) can
/// get wrong: what is raised outside the focus file's text or inside an
/// instantiation, which it may miss, and the warnings about unused
/// declarations, which it also raises for those only a skipped body uses.
auto skipped_diagnostics(CompilationUnit& unit) -> std::vector<FileDiagnostic>;

/// A warning from clice itself about the file as a whole, on its first
/// character.
FileDiagnostic file_warning(std::string message);

/// The LSP form of `diagnostic`. A client that takes no related
/// information gets the notes in the message.
protocol::Diagnostic to_protocol(const FileDiagnostic& diagnostic, bool related_information);

}  // namespace clice::feature
