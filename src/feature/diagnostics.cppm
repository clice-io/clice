module clice:feature.diagnostics;

import :command.table;
import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :support.prelude;
import :syntax.position;
import :syntax.token;

namespace clice::feature {

/// Corresponds to the `[diagnostics]` section in clice.toml.
KANNOTATE_TYPE(struct DiagnosticsOptions, presence_all = optional);
struct DiagnosticsOptions {
    KANNOTATE(&clang_tidy,
              description =
                  "Run clang-tidy on open files and offer its fixes, with "
                  "the checks the nearest `.clang-tidy` configures (a "
                  "small default set without one). Checks too slow for "
                  "an editor are left out unless `slow_clang_tidy_checks` "
                  "is `true`, those unreliable on code being edited "
                  "always are; files `clice lint` does not check get "
                  "none. `NOLINT` comments also silence compiler warnings "
                  "only while this runs.")
    bool clang_tidy = true;

    KANNOTATE(&slow_clang_tidy_checks,
              description =
                  "Also run on open files the checks the `.clang-tidy` "
                  "enables that are too slow for an editor or not measured "
                  "for one (`misc-const-correctness` doubles the time a "
                  "large file takes to compile): every compile of the file "
                  "waits for them. Unset, clice names such checks once "
                  "when it leaves them out; `false` leaves them out "
                  "silently. `clice lint` runs them either way.")
    std::optional<bool> slow_clang_tidy_checks;

    KANNOTATE(&suppress,
              description =
                  "Diagnostics open files do not show, by their code: a "
                  "compiler warning's option (`-Wunused-variable`), "
                  "clang's name for any other diagnostic "
                  "(`err_undeclared_var_use`), a clang-tidy check, or "
                  "one of clice's own warnings "
                  "(`toolchain-query-failed`). "
                  "For code clang gets wrong: everything else works on "
                  "the file as before, and `clice lint` still reports "
                  "them.")
    std::vector<std::string> suppress;
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

/// A place a diagnostic's note points at: bytes of the file at `path`, or
/// of the text compiled when `path` is empty — the diagnostic's own file.
struct DiagnosticNote {
    std::string path;
    LocalSourceRange range;
    std::string message;
};

/// A diagnostic as shown on one file, at bytes of the text compiled: the
/// compiler's and clang-tidy's of its compile, and clice's own about the
/// file. Each client gets it in the shape and the positions it takes
/// (to_protocol).
struct FileDiagnostic {
    /// The file's first character unless placed.
    LocalSourceRange range = {0, 0};
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
auto diagnostics(CompilationUnit& unit) -> std::vector<FileDiagnostic>;

/// Of the diagnostics of a compile parsing every body, those a compile
/// skipping the bodies of the headers (CompilationParams::skip_bodies) can
/// get wrong: what is raised outside the focus file's text or inside an
/// instantiation, which it may miss, and the warnings about unused
/// declarations, which it also raises for those only a skipped body uses.
auto skipped_diagnostics(CompilationUnit& unit) -> std::vector<FileDiagnostic>;

/// How many errors the compiler raised in any of its files, those of an
/// includer's context it compiles a header in among them, which its focus
/// file's diagnostics leave out; clang-tidy's findings, whatever their
/// level, are no part of whether code compiles. The warning that a struct a prototype
/// names first is visible in that prototype alone counts as one: the
/// includer declares the struct, and C compiles the header without it, its
/// functions then taking a type no caller has.
std::size_t count_errors(CompilationUnit& unit);

/// count_errors of diagnostics already gathered: those of a PCH's build.
std::size_t count_errors(llvm::ArrayRef<FileDiagnostic> diagnostics);

/// A warning from clice itself about the file as a whole, on its first
/// character.
FileDiagnostic file_warning(std::string message);

/// What a compile of a file raised, and the facts clice's own warnings
/// about the compile are built from (shown_diagnostics).
struct CompileDiagnostics {
    /// How the compile command was made: a guessed one may explain the
    /// includes not found.
    CommandSource source = CommandSource::Database;

    /// The file whose command it is, as the user knows it: the unit the
    /// command was borrowed from, the host a header compiles in; empty for
    /// the file's own.
    std::string command_of;

    /// The file lies outside its project's root: no database of the
    /// project would list it.
    bool outside_project = false;

    /// The directory the command runs in, when it does not exist: a stale
    /// database, or one written on another machine; empty otherwise.
    std::string missing_directory;

    /// What the compile raised: the compiler's and clang-tidy's
    /// diagnostics, and clice's own about the compile; on a failure before
    /// parsing, why it failed.
    std::vector<FileDiagnostic> own;

    /// The warnings of unused declarations the compile raised while it
    /// skipped the function bodies of the headers (take_unconfirmed), held
    /// back for the background index's compile to confirm.
    std::vector<FileDiagnostic> unconfirmed;

    /// What the background index's compile of the same text reported that
    /// this one cannot report or has held back (confirmed_skipped), shown
    /// after the compile's own.
    std::vector<FileDiagnostic> skipped;

    /// The host whose includer context the compile needed but could not
    /// rebuild, as the user knows it; empty otherwise.
    std::string unmatched_host;

    /// Why the compiler could not be queried, which leaves the command
    /// clang's own reading of it; empty when it answered.
    std::string query_failure;
};

/// The diagnostics a file shows of its compile: its own, then the skipped
/// ones, less those `options` suppress and, of those moved to one place
/// from elsewhere, all but the first error and the first warning; behind
/// the warnings at its top saying what they may miss: a command clice
/// guessed, an includer context it could not rebuild, a compiler it could
/// not query.
auto shown_diagnostics(const CompileDiagnostics& diagnostics, const DiagnosticsOptions& options)
    -> std::vector<FileDiagnostic>;

/// The compile's diagnostics behind the ones its PCH's build raised in the
/// preamble, which the parse consuming the PCH never raises again — those
/// of the command line it does, and they appear once.
auto with_preamble(std::vector<FileDiagnostic> own, std::vector<FileDiagnostic> preamble)
    -> std::vector<FileDiagnostic>;

/// The warnings a compile skipping the function bodies of the headers
/// cannot trust: those of unused declarations, which a use in a skipped
/// body would silence; taken out of `diagnostics`.
auto take_unconfirmed(std::vector<FileDiagnostic>& diagnostics) -> std::vector<FileDiagnostic>;

/// Whether two diagnostics show the same: one message at one place.
bool same_diagnostic(const FileDiagnostic& left, const FileDiagnostic& right);

/// Of the diagnostics a full compile of the same text reported that a
/// compile skipping the bodies of the headers can get wrong
/// (skipped_diagnostics), those the file shows beside that compile's: an
/// unused declaration's warning it held back, now confirmed, and the rest
/// it did not raise itself.
auto confirmed_skipped(llvm::ArrayRef<FileDiagnostic> skipped, const CompileDiagnostics& compiled)
    -> std::vector<FileDiagnostic>;

/// Why an imported module did not build: from the imported module down,
/// the modules whose builds failed in turn, and the error the last one
/// failed on, or the index in `chain` of the module the last one's import
/// cycles back to.
struct ModuleFailure {
    std::vector<std::string> chain;

    std::string error;

    /// Where `error` is, when the build placed it.
    std::optional<DiagnosticNote> error_at;

    /// How many more errors the last one's build met.
    std::uint32_t more_errors = 0;

    std::optional<std::size_t> cycle;
};

/// clang reports the import of a module whose build failed as not found:
/// `message`, when it is that report for one of `failures`, says instead
/// which module did not build, why, and through which imports. The failure
/// it names, none for any other message.
const ModuleFailure* explain_failed_import(std::string& message,
                                           llvm::ArrayRef<ModuleFailure> failures);

/// explain_failed_import over the diagnostics of a compile, each then
/// clice's (`module-not-built`, `module-cycle`), with the place of the
/// error the module failed on.
void explain_failed_imports(std::vector<FileDiagnostic>& diagnostics,
                            llvm::ArrayRef<ModuleFailure> failures);

/// An import of a module that several files declare, resolved to the
/// nearest of them.
struct ModuleChoice {
    std::string module;

    /// The files declaring it, as the user knows them: the one taken and
    /// the ones passed over.
    std::string chosen;
    std::vector<std::string> others;

    /// Where in the compiled text the import is written; past its end for
    /// an import the file does not write itself.
    std::uint32_t offset = 0;
};

/// The warning on an import saying which of the files declaring its
/// module it took; on the file's top for an import it does not write.
FileDiagnostic ambiguous_module(const ModuleChoice& choice, llvm::StringRef text);

/// The warning at a file's top that clang-tidy does not run on it while
/// its configuration fails to parse, each place the error names a note.
FileDiagnostic invalid_tidy_config(const tidy::TidyResolution& resolution);

/// What a compile reports in place of an index that could not be handed
/// over.
FileDiagnostic index_unavailable(llvm::StringRef cause);

/// The kinds of work done for a document on a worker, each with a failure
/// record of its own: a crashing hover stops hover alone.
enum class RequestKind : std::uint8_t {
    Compile,
    /// The compile's clang-tidy pass: a crash under it retries the compile
    /// without the pass.
    Tidy,
    /// Building the document's preamble.
    PCH,
    /// Building a module the document imports.
    PCM,
    DocumentLink,
    FoldingRange,
    DocumentSymbol,
    InlayHint,
    CodeAction,
    SelectionRange,
    Completion,
    SignatureHelp,
    Format,
    SemanticTokens,
    Hover,
};

/// The kind as logs name it: its enumerator in lower snake case
/// (`semantic_tokens`).
inline std::string request_name(RequestKind kind) {
    return kotatsu::naming::normalize_to_lower_snake(kotatsu::meta::enum_name(kind));
}

/// A crash that stops work on a document.
struct CrashNote {
    RequestKind kind;

    /// How the worker died.
    llvm::StringRef cause;

    /// Crashes in a row.
    unsigned count = 0;

    /// Only a save retries: the crashes in a row ran out.
    bool save_only = false;
};

/// Where clice takes bug reports.
constexpr inline llvm::StringRef issue_tracker = "https://github.com/clice-io/clice/issues";

/// The warning at a file's top telling what of it a crash stopped, what
/// brings it back, and where the crash report is: in `reports`, the
/// session's log directory, or the server's log when it keeps none.
FileDiagnostic crash_note(const CrashNote& note, llvm::StringRef reports);

/// The warning at a file's top saying that its compile, which reported
/// errors, read `files` from disk while their documents hold unsaved text:
/// a compile never reads another document's buffer.
FileDiagnostic unsaved_inputs_note(llvm::ArrayRef<std::string> files);

/// The LSP form of `diagnostic`, shown on the file at `path` whose
/// compiled text `positions` maps, its positions counted in `encoding`; a
/// note in another file is placed in the text `note_positions` reads of it,
/// left out when it cannot read it or the note lies past its end. A client
/// that takes no related information gets the notes in the message.
protocol::Diagnostic to_protocol(
    const FileDiagnostic& diagnostic,
    bool related_information,
    PositionEncoding encoding,
    llvm::StringRef path,
    const PositionMap& positions,
    llvm::function_ref<std::optional<PositionMap>(llvm::StringRef path)> note_positions);

}  // namespace clice::feature
