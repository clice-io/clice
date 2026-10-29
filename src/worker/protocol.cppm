module clice:worker.protocol;

import :compile.compilation;
import :config.config;
import :feature.code_actions;
import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.inlay_hints;
import :feature.semantic_tokens;
import :index.symbol;
import :support.prelude;
import :syntax.header_context;
import :syntax.position;
import :syntax.token;
import :vfs.file_system;

namespace clice::worker {

/// Error codes attached to master-side dispatch failures. They mark expected
/// operational conditions — memory-pressure preemption, worker deaths and
/// restart windows — as opposed to real IPC breakage: callers must not
/// classify them as anomalies (see support/anomaly.cppm). The death itself is
/// already reported as a WorkerCrash anomaly by the pool.
///
/// A death fails every request in flight on the worker; the pool tells them
/// apart by the request the dying worker named (see crash_tag), so only the
/// request that crashed it is blamed.
namespace dispatch_errc {

/// The request was deliberately cancelled: by its caller, or by the pool
/// taking its slot back for interactive work.
constexpr inline protocol::integer cancelled =
    static_cast<protocol::integer>(protocol::ErrorCode::RequestCancelled);

/// No live worker could take the request; it was never dispatched.
constexpr inline protocol::integer worker_unavailable = -33000;

/// This request killed its worker: the dying worker named it, or it ran
/// past the pool's deadline. The message says how the worker died.
constexpr inline protocol::integer worker_crashed = -33001;

/// The worker died of another request's crash, or died naming no request
/// while several were in flight: this one is blameless and safe to resend.
constexpr inline protocol::integer worker_lost = -33003;

/// The worker died naming no request while this one ran alone on it —
/// killed from outside (the OOM killer, a user), or crashed where it could
/// not report. Resend once; a request that dies this way twice is blamed.
constexpr inline protocol::integer worker_died = -33004;

/// A stateful worker no longer holds the document the query is about (its
/// LRU evicted it): recompile and ask again.
constexpr inline protocol::integer document_unloaded = -33005;

/// The worker's memory ran out: the pool killed it to free memory, or an
/// allocation failed in it. The request is blameless; Error::data holds
/// "memory", the bytes the worker had reached, and "alone", whether no
/// other request ran beside it — then it does not fit this machine.
constexpr inline protocol::integer out_of_memory = -33006;

}  // namespace dispatch_errc

/// True when a dispatch failure is an expected operational condition rather
/// than clice infrastructure breakage.
inline bool is_operational_error(const protocol::Error& error) {
    return error.code == dispatch_errc::cancelled ||
           error.code == dispatch_errc::worker_unavailable ||
           error.code == dispatch_errc::worker_crashed ||
           error.code == dispatch_errc::worker_lost || error.code == dispatch_errc::worker_died ||
           error.code == dispatch_errc::document_unloaded ||
           error.code == dispatch_errc::out_of_memory;
}

/// The stderr line a dying worker writes to name the request it was
/// running: this prefix, then the request's crash_tag.
constexpr inline std::string_view crashed_in_marker = "clice worker crashed in: ";

/// How a dying worker names a request, and how the master recognizes its
/// own request in that line: the method and the file.
/// Requests a stateful worker runs side by side differ in one of them
/// unless they are the same work on the same document.
template <typename Params>
std::string crash_tag(const Params& params) {
    std::string tag(protocol::RequestTraits<Params>::method);
    tag += ' ';
    if constexpr(requires { params.path; }) {
        tag += params.path;
    } else {
        tag += params.file;
    }
    return tag;
}

/// Identity of the worker incarnation a crashed request died with, carried
/// in Error::data. One process death fails every request in flight on it;
/// a failure record (FailureRecord) counts each identity once, and the pool
/// finds the slot it names again (WorkerPool::exempt_death).
inline codec::dyn::Value death_identity(std::size_t index, unsigned generation, bool stateful) {
    return std::format("{}:{}:{}", stateful ? "sf" : "sl", index, generation);
}

/// The death identity attached to a worker death's error; empty when the
/// error carries none (locally synthesized failures).
inline std::string_view death_of(const protocol::Error& error) {
    if(error.data.has_value()) {
        return error.data->get_string().value_or(std::string_view{});
    }
    return {};
}

/// True for a worker link that closed under a request (broken pipe, dead
/// process), as opposed to an error the remote handler returned.
inline bool is_transport_error(const protocol::Error& error) {
    return error.code == static_cast<protocol::integer>(protocol::ErrorCode::ConnectionClosed);
}

/// Request the hover card at a point of an open file's AST, before it is
/// rendered (see feature::hover_info).
struct HoverParams {
    std::string path;
    uint32_t offset = 0;

    /// The workspace config, carried whole on every request — the worker
    /// holds no config state and a config change simply shows up on the
    /// next request. Features read their own section; no per-feature
    /// forwarding field is ever added here.
    Config config;
};

/// Parameters for stateful compilation (builds AST, publishes diagnostics).
struct CompileParams {
    std::string path;
    int version;
    std::string text;
    std::string directory;
    /// See CompilationInput::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    /// Conditional levels left open by the PCH's preamble (1 = branch
    /// inactive), seeding the main compile's inactive-region scan.
    std::vector<std::uint8_t> open_conditionals;

    /// Inactive regions of the preamble share (byte-offset pairs up to the
    /// PCH bound), read from the PCH's pch.idx index. The worker only
    /// scans past the bound, yet serves semantic tokens for the whole file.
    std::vector<std::uint32_t> preamble_inactive_regions;

    /// See TURunParams::index_output_path.
    std::string index_output_path;

    /// See TURunParams::known_instantiations.
    std::vector<std::uint64_t> known_instantiations;

    /// See TURunParams::standard_library_instantiations.
    bool standard_library_instantiations = false;

    /// Run clang-tidy over the parse with this frozen configuration.
    std::optional<tidy::TidyParams> tidy;

    /// See CompilationParams::skip_bodies.
    bool skip_bodies = false;
};

/// Outcome of a stateful compile. Anything but `Done` is a non-result: the
/// master must not commit the result to the session or record
/// dependencies — doing so freezes the document on a product that never
/// existed. A setup failure's diagnostics say why it failed.
enum class CompileStatus : uint8_t {
    /// The parse produced a usable product — a complete AST, or a fatal
    /// error whose diagnostics describe the user's code.
    Done,
    /// The parse was interrupted (superseded round).
    Cancelled,
    /// The frontend failed before parsing began: bad invocation, or a
    /// prebuilt input (PCH/PCM) clang could not read. Whether the consumed
    /// PCH is suspect is a separate signal (pch_suspect).
    SetupFail,
};

struct CompileResult {
    CompileStatus status = CompileStatus::Done;

    /// The parse failed and its diagnostics point at the consumed PCH — they
    /// name the blob's path, or they are AST-deserialization errors naming
    /// no other prebuilt input. Holds whether the reader rejected the blob
    /// at setup or hit a fatal error past it (a Done status with real,
    /// publishable diagnostics). Either way the artifact is the culprit:
    /// the master should retract the pair and rebuild instead of trusting
    /// the corrupt bytes until the preamble changes.
    bool pch_suspect = false;

    int version;
    std::vector<feature::FileDiagnostic> diagnostics;
    /// Milliseconds since epoch, sampled before the compile started. Files
    /// whose mtime is past this moment may differ from what the build read.
    std::int64_t build_at = 0;
    std::vector<DepFile> deps;
    /// Serialized TUIndex of the focus file.
    std::vector<std::byte> tu_index_data;

    /// See TURunResult::index_in_file.
    bool index_in_file = false;
};

/// Build a PCH (and its paired pch.idx index) from preamble content, or
/// from a whole file the build precompiles for other files.
struct BuildPCHParams {
    std::string file;
    std::string directory;
    /// See CompilationInput::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

    /// The preamble content, remapped over the file.
    std::string content;
    uint32_t preamble_bound = UINT32_MAX;

    /// Build the PCH of the whole file as it is on disk: a precompiled
    /// header the build declares (Build::pch_unit). content and
    /// preamble_bound are unused.
    bool whole_file = false;

    /// The precompiled header the build declares for the command, built by
    /// clice: the preamble's PCH is built over it and chains onto it. Empty
    /// for none.
    std::string pch;

    /// Every built module's PCM (module name -> artifact path), where the
    /// preamble's imports load theirs from.
    std::unordered_map<std::string, std::string> pcms;

    /// See CompilationParams::skip_bodies.
    bool skip_bodies = false;

    /// Tmp path allocated by the master's store; the master commits
    /// (fsync + atomic rename) after the worker reports success.
    std::string output_path;

    /// Tmp path for the pch.idx index, allocated alongside output_path.
    /// The worker serializes the preamble's index and feature state into
    /// it; the master commits both blobs together.
    std::string index_output_path;
};

/// Build the PCM of a unit providing a module.
struct BuildPCMParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;

    std::string module_name;

    /// Transitive PCM dependencies (module name -> artifact path).
    std::unordered_map<std::string, std::string> pcms;

    /// The precompiled header the build declares for the command, built by
    /// clice; empty for none.
    std::string pch;

    /// Tmp path allocated by the master's store (see BuildPCHParams).
    std::string output_path;
};

/// One whole-TU run: a single parse serving the products the frozen plan
/// names — the full index, a clang-tidy pass, or both.
struct TURunParams {
    std::string file;
    std::string directory;
    /// See CompilationInput::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;

    /// PCM dependencies for TUs that import modules.
    std::unordered_map<std::string, std::string> pcms;

    /// The precompiled header the build declares for the command, built by
    /// clice; empty for none.
    std::string pch;

    /// Products of the run: the index, the errors the compile reports, the
    /// diagnostics an editor skipping header bodies misses, and a clang-tidy
    /// pass under this frozen configuration.
    bool index = false;
    bool errors = false;
    bool skipped_diagnostics = false;
    std::optional<tidy::TidyParams> tidy;

    /// Variant identities the master stores for the files this TU is
    /// expected to include, sorted (see index::TUIndexOptions).
    std::vector<std::uint64_t> known_variants;

    /// The keys of the instantiations the master holds for those files,
    /// sorted (see index::TUIndexOptions).
    std::vector<std::uint64_t> known_instantiations;

    /// Index the standard library's own instantiations and expansions (see
    /// index::TUIndexOptions).
    bool standard_library_instantiations = false;

    /// Where the worker writes an index too large for an IPC message: a
    /// transfer path of the master's store, empty when it has none.
    std::string index_output_path;
};

/// Code completion over unsaved buffer content.
struct CompletionParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

    std::string text;
    uint32_t offset = 0;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    /// The workspace config, carried whole — the worker holds no config
    /// state and a config change simply shows up on the next request.
    Config config;

    feature::CompletionClient client;

    /// What the edits' ranges count in: the client's position encoding.
    PositionEncoding encoding = PositionEncoding::UTF16;

    /// The names of other namespaces the index found for the typed prefix.
    std::vector<feature::ScopeCandidate> candidates;

    /// The index found more of them: the list is incomplete, so the editor
    /// asks again as the prefix grows.
    bool more_candidates = false;
};

/// A completion's reply: the list as the client takes it, and where the
/// `#include` goes that the items marked with their symbol need
/// (feature::Completion).
struct CompletionResult {
    codec::RawValue list{"null"};
    std::optional<feature::IncludePlace> include;
};

/// Signature help over unsaved buffer content; same inputs as completion.
struct SignatureHelpParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

    std::string text;
    uint32_t offset = 0;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    Config config;

    /// The label of the signature the client shows active, empty when the
    /// popup is not open.
    std::string selected;
};

/// Format a document (or a byte range of it) with clang-format.
struct FormatParams {
    std::string file;
    std::string text;
    LocalSourceRange range;  ///< Invalid range = full document.
};

/// The edits of the request's text, or why the file's style cannot be
/// read.
struct FormatResult {
    std::vector<TextReplacement> edits;
    std::string error;
};

/// Result of an artifact build (PCH or PCM).
struct ArtifactBuildResult {
    bool success = true;
    std::string error;
    /// On failure: whether `error` carries user-code compile errors. A failure
    /// without user errors indicates clice infrastructure breakage (anomaly).
    bool has_user_errors = false;

    /// The first of the user errors: the one a module's importers name as
    /// why it did not build.
    std::string first_error;

    /// The tmp path the artifact was written to.
    std::string output_path;

    /// Milliseconds since epoch, sampled before the build started. Files
    /// whose mtime is past this moment may differ from what the build read.
    std::int64_t build_at = 0;
    /// What the build read and looked for — on failure too: a build that
    /// failed on the user's errors fails again until one of them changes.
    std::vector<DepFile> deps;
};

/// A message where it is, located for CLI presentation: 1-based line and
/// column, the column counting bytes like the compiler's; a message no
/// file holds (the command line's) has neither.
struct LocatedMessage {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string message;

    auto operator<=>(const LocatedMessage&) const = default;
};

struct TidyDiagnostic {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    /// Error (warning otherwise): the check is in WarningsAsErrors.
    bool error = false;

    /// The compiler's own error, which no header filter or lint set leaves
    /// out.
    bool compiler_error = false;

    std::string message;

    /// Check name, e.g. "bugprone-integer-division".
    std::string check;

    /// The notes clang-tidy attached ("previous definition is here").
    std::vector<LocatedMessage> notes;
};

struct TURunResult {
    bool success = true;
    std::string error;

    /// The errors a completed run compiled with (plan product `errors`).
    std::vector<LocatedMessage> errors;

    /// Serialized TUIndex, merged by the master (plan product `index`).
    std::vector<std::byte> tu_index_data;

    /// The index went to index_output_path instead of tu_index_data.
    bool index_in_file = false;

    /// Findings of the tidy pass (plan product `tidy`).
    std::vector<TidyDiagnostic> tidy_diagnostics;

    /// The diagnostics a compile skipping the function bodies of the
    /// headers can get wrong (feature::skipped_diagnostics) (plan product
    /// `skipped_diagnostics`).
    std::vector<feature::FileDiagnostic> skipped_diagnostics;

    /// The hash of the text skipped_diagnostics describe.
    std::uint64_t text_hash = 0;
};

/// Preprocess a unit for its include tree: where its compile enters each
/// file (see PreprocessedTree).
struct IncludeTreeParams {
    std::string file;
    std::string directory;
    /// See CompilationInput::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files on disk the command hides (Resolution::hidden).
    vfs::HiddenFiles hidden;
};

struct IncludeTreeResult {
    bool success = true;
    std::string error;

    /// The files the run read, the unit's own last, with the hash of the
    /// bytes it read of each.
    std::vector<std::string> paths;
    std::vector<std::uint64_t> content_hashes;

    /// The tree, each node's file an index into `paths`.
    std::vector<index::IncludeNode> nodes;

    /// Every file the run read or looked for: its includes, what its
    /// `__has_include` checks found, the places its failed lookups looked.
    std::vector<DepFile> deps;

    /// Milliseconds since epoch, sampled before the run started.
    std::int64_t build_at = 0;
};

/// Request the document links of an open file's AST. Only the focus file's
/// region is covered: the preamble is compiled into the PCH, and its links
/// live in the PCH's pch.idx index (spliced in by the master).
struct DocumentLinkParams {
    std::string path;
};

/// Request the folding ranges of an open file's AST. Unlike the links they
/// cover the preamble too: a preamble holds only directives, whose folds
/// come from a lexical scan of the whole file.
struct FoldingRangeParams {
    std::string path;
};

/// Request the code actions of an open file's AST on a byte range of its
/// text: fully computed against the worker's AST, index requests included
/// (see feature::CodeAction).
struct CodeActionParams {
    std::string path;
    LocalSourceRange range;
};

/// Request the selection ranges of an open file's AST around byte offsets
/// of its text, one chain per offset (see feature::selection_ranges).
struct SelectionRangeParams {
    std::string path;
    std::vector<std::uint32_t> offsets;
};

/// Request the inlay hints of an open file's AST within a byte range of its
/// text (see feature::inlay_hints).
struct InlayHintParams {
    std::string path;
    LocalSourceRange range;

    /// The workspace config, carried whole — the worker holds no config
    /// state and a config change simply shows up on the next request.
    Config config;
};

/// Request the semantic tokens of an open file's AST (see
/// feature::semantic_tokens).
struct SemanticTokensParams {
    std::string path;

    /// The workspace config, carried whole — the worker holds no config
    /// state and a config change simply shows up on the next request.
    Config config;
};

/// Request the outline of an open file's AST (see feature::document_symbols).
struct DocumentSymbolParams {
    std::string path;

    /// The workspace config, carried whole — the worker holds no config
    /// state and a config change simply shows up on the next request.
    Config config;
};

struct EvictParams {
    std::string path;
};

struct EvictedParams {
    std::string path;
};

/// Test hook (see BuildProbe::gate): park the work of the next request
/// whose crash_tag is `tag` where it starts, until released.
struct GateParams {
    std::uint64_t id = 0;
    std::string tag;
};

/// Test hook: the gate parked a request's work.
struct GateParkedParams {
    std::uint64_t id = 0;
};

/// Test hook: let the work the gate parked go on, or drop the gate.
struct GateReleaseParams {
    std::uint64_t id = 0;
};

/// Whether a request builds — a compile, a PCH or PCM, an indexing run:
/// work whose time grows with the translation unit, where a query's never
/// should.
template <typename Params>
constexpr inline bool is_build =
    std::same_as<Params, CompileParams> || std::same_as<Params, BuildPCHParams> ||
    std::same_as<Params, BuildPCMParams> || std::same_as<Params, TURunParams> ||
    std::same_as<Params, IncludeTreeParams>;

}  // namespace clice::worker

namespace kota::ipc::protocol {

template <>
struct RequestTraits<clice::worker::CompileParams> {
    using Result = clice::worker::CompileResult;
    constexpr inline static std::string_view method = "clice/worker/compile";
};

template <>
struct RequestTraits<clice::worker::HoverParams> {
    using Result = std::optional<clice::feature::HoverInfo>;
    constexpr inline static std::string_view method = "clice/worker/hover";
};

template <>
struct RequestTraits<clice::worker::DocumentLinkParams> {
    using Result = std::vector<clice::index::DocumentLink>;
    constexpr inline static std::string_view method = "clice/worker/documentLink";
};

template <>
struct RequestTraits<clice::worker::FoldingRangeParams> {
    /// Empty without an AST: the client then folds by its own means.
    using Result = std::optional<std::vector<clice::feature::FoldingRange>>;
    constexpr inline static std::string_view method = "clice/worker/foldingRange";
};

template <>
struct RequestTraits<clice::worker::CodeActionParams> {
    using Result = std::vector<clice::feature::CodeAction>;
    constexpr inline static std::string_view method = "clice/worker/codeAction";
};

template <>
struct RequestTraits<clice::worker::SelectionRangeParams> {
    /// Empty without an AST.
    using Result = std::vector<std::vector<clice::LocalSourceRange>>;
    constexpr inline static std::string_view method = "clice/worker/selectionRange";
};

template <>
struct RequestTraits<clice::worker::SemanticTokensParams> {
    /// Empty without an AST.
    using Result = std::optional<std::vector<clice::feature::SemanticToken>>;
    constexpr inline static std::string_view method = "clice/worker/semanticTokens";
};

template <>
struct RequestTraits<clice::worker::DocumentSymbolParams> {
    /// Empty without an AST.
    using Result = std::optional<std::vector<clice::feature::DocumentSymbol>>;
    constexpr inline static std::string_view method = "clice/worker/documentSymbol";
};

template <>
struct RequestTraits<clice::worker::InlayHintParams> {
    /// Empty without an AST.
    using Result = std::vector<clice::feature::InlayHint>;
    constexpr inline static std::string_view method = "clice/worker/inlayHint";
};

template <>
struct RequestTraits<clice::worker::BuildPCHParams> {
    using Result = clice::worker::ArtifactBuildResult;
    constexpr inline static std::string_view method = "clice/worker/buildPch";
};

template <>
struct RequestTraits<clice::worker::BuildPCMParams> {
    using Result = clice::worker::ArtifactBuildResult;
    constexpr inline static std::string_view method = "clice/worker/buildPcm";
};

template <>
struct RequestTraits<clice::worker::TURunParams> {
    using Result = clice::worker::TURunResult;
    constexpr inline static std::string_view method = "clice/worker/tuRun";
};

template <>
struct RequestTraits<clice::worker::IncludeTreeParams> {
    using Result = clice::worker::IncludeTreeResult;
    constexpr inline static std::string_view method = "clice/worker/includeTree";
};

template <>
struct RequestTraits<clice::worker::CompletionParams> {
    using Result = clice::worker::CompletionResult;
    constexpr inline static std::string_view method = "clice/worker/completion";
};

template <>
struct RequestTraits<clice::worker::SignatureHelpParams> {
    using Result = codec::RawValue;
    constexpr inline static std::string_view method = "clice/worker/signatureHelp";
};

template <>
struct RequestTraits<clice::worker::FormatParams> {
    using Result = clice::worker::FormatResult;
    constexpr inline static std::string_view method = "clice/worker/format";
};

template <>
struct NotificationTraits<clice::worker::EvictParams> {
    constexpr inline static std::string_view method = "clice/worker/evict";
};

template <>
struct NotificationTraits<clice::worker::EvictedParams> {
    constexpr inline static std::string_view method = "clice/worker/evicted";
};

template <>
struct NotificationTraits<clice::worker::GateParams> {
    constexpr inline static std::string_view method = "clice/worker/gate";
};

template <>
struct NotificationTraits<clice::worker::GateParkedParams> {
    constexpr inline static std::string_view method = "clice/worker/gateParked";
};

template <>
struct NotificationTraits<clice::worker::GateReleaseParams> {
    constexpr inline static std::string_view method = "clice/worker/gateRelease";
};

}  // namespace kota::ipc::protocol
