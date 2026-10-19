module;

#include "modules/prelude.h"

module clice:project.command_resolver;

import :command.command;
import :project.project;

namespace clice {

/// A header the artifact records persist as needing an includer's context.
/// Mirrors the on-disk JSON layout field for field — changing it changes
/// the format.
struct ContextHeaderEntry {
    /// Index into the records' path table.
    std::uint32_t file;

    /// The header contents the result was scored on.
    std::uint64_t content_hash;
};

/// What one resolution asks for beyond the file itself. The editor fields
/// are filled by EditorContext for editor-facing compiles of open files;
/// a background resolution (indexing, batch runs, `clice inspect`) leaves
/// them empty and so depends on nothing but the project on disk.
struct CommandRequest {
    /// Per-run additions to the database driver command (a lint plan's
    /// clang-tool extra args), applied before toolchain resolution so the
    /// driver interprets them. Unlike config rule appends they are never
    /// NVCC-translated; prepends land right after the binary name, appends
    /// win over rule appends.
    llvm::ArrayRef<std::string> extra_prepend;
    llvm::ArrayRef<std::string> extra_append;

    /// The user's pin for the file (clice/switchContext).
    const Selection* selection = nullptr;

    /// Resolved header contexts to reuse and to fill: a still-valid entry
    /// is reused, a stale one replaced.
    llvm::DenseMap<Fid, HeaderContext>* header_contexts = nullptr;
};

/// Pick the candidate matching a pinned command (multi-configuration files
/// and hosts), defaulting to the build's first command. `paths` are the
/// files whose edits the published hash was computed with.
Candidate pick_pinned_config(Project& project,
                             Fid file,
                             llvm::ArrayRef<Candidate> candidates,
                             llvm::ArrayRef<CanonicalRef> paths,
                             llvm::StringRef language_path,
                             llvm::StringRef pinned_hash,
                             llvm::StringRef pinned_base);

/// `command`, one of `host`'s, as `host` lends it to `header`: edited by
/// the rules of both files and a run's extras, compiled as the host's
/// language.
CommandRef lent_command(Project& project,
                        Fid header,
                        Fid host,
                        ConfigID command,
                        llvm::ArrayRef<std::string> extra_prepend = {},
                        llvm::ArrayRef<std::string> extra_append = {});

/// The command `host` lends `header`: of the host's commands fit for the
/// header, the pinned one (else the first), as lent_command lends it;
/// nullopt when no command fits.
std::optional<CommandRef> host_command(Project& project,
                                       Fid header,
                                       Fid host,
                                       llvm::StringRef pinned_hash,
                                       llvm::StringRef pinned_base,
                                       llvm::ArrayRef<std::string> extra_prepend = {},
                                       llvm::ArrayRef<std::string> extra_append = {});

/// How a file's command was resolved.
struct Resolution {
    CommandSource source = CommandSource::Fallback;

    /// The host whose command an IncludeGraph resolution borrowed;
    /// invalid for every other source.
    Fid host;

    /// The command selection behind the rendered arguments, for
    /// structured consumers (search config, language queries).
    CommandRef ref;

    /// The header context the arguments name (-include of its prefix),
    /// which exists only in memory: every compile under them must be
    /// served its files. Null when nothing was synthesized.
    std::shared_ptr<const SynthesizedContext> synthesized;

    /// The host whose includer context the header needed but whose include
    /// chain the synthesis could not follow: the command fell back past it.
    Fid unmatched_host;

    /// The host whose include tree could change this resolution: the
    /// synthesis followed the lexical chain from it, for want of a tree
    /// telling where its compile enters the header (see enterings), or
    /// could not follow the chain at all.
    Fid tree_wanted;
};

/// Composes a file's final compile command from the project on disk.
///
/// The build (`Build`) says what a file compiles as; the hosting layer
/// (`default_host`) says which unit stands in for a header without a
/// command of its own. `resolve_command` applies them in order — own
/// entry, host, default command, lender, builtin — then the rule edits
/// and a run's extras; an editor request layers the user's pin and its
/// cached header contexts on top. Around the host branch it owns the
/// header-context synthesis (in-memory files restoring the includer's
/// preprocessor state, see SynthesizedContext) and the self-containment
/// results that decide it, persisted in the artifact records.
class CommandResolver {
public:
    explicit CommandResolver(Project& project) : project(project) {}

    /// Effective self-containment mode for a header. X-macro style
    /// extensions are non-self-contained by construction; otherwise use
    /// the persisted result. Entering or leaving SelfContained moves
    /// Project::context_epoch: the header's context listing merges hosts
    /// only then (ContextService::contexts). Only NeedsContext is ever persisted — a
    /// "self-contained" impression is session-local and re-evaluated when
    /// compile inputs change, so it can never go stale.
    HeaderMode header_mode(Fid path_id) const;

    /// Drop an in-memory SelfContained result (never a persisted
    /// NeedsContext) so the next compile re-runs the trial.
    void forget_self_contained(Fid path_id);

    /// Record a header trial's result. NeedsContext carries the content
    /// hash it was scored on so a stale result is dropped on cache load;
    /// scored with no disk observation (hash 0) it stays session-local.
    /// Marks the artifact records dirty when the persisted slice changes.
    void record_header_mode(Fid path_id, HeaderMode mode, std::uint64_t content_hash = 0);

    /// Drop a header's result entirely (its content changed); the next
    /// compile re-earns it. Marks the artifact records dirty when a
    /// persisted result is dropped.
    void reset_header_mode(Fid path_id);

    /// Fill the artifact records' headers that need a context (content-hash
    /// gated at load). @param intern_id maps a runtime path id into the
    /// blob's path table; interning order is part of the on-disk format.
    void dump_context_headers(std::vector<ContextHeaderEntry>& headers,
                              llvm::function_ref<std::uint32_t(Fid)> intern_id) const;

    /// Restore the headers that need a context. @param file_of maps a path
    /// table index back to the file it names (nullopt when the index is
    /// invalid).
    void load_context_headers(llvm::ArrayRef<ContextHeaderEntry> headers,
                              llvm::function_ref<std::optional<Fid>(std::uint32_t)> file_of);

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: the pinned host, the file's own command, a header
    /// context through the include graph, a default command, a lender and
    /// finally the builtin command — so it always succeeds. Emits a
    /// per-file decision log (tiers tried, tier hit, command hash).
    Resolution resolve_command(Fid path_id,
                               std::string& directory,
                               std::vector<std::string>& arguments,
                               const CommandRequest& request = {});

    /// Build::pch_unit for the command `resolution` and `request` gave
    /// `path_id`: edited by the rules of the files it was resolved for (a
    /// borrowing header's and its host's) and the request's extras.
    std::optional<CommandRef> pch_unit(Fid path_id,
                                       const Resolution& resolution,
                                       const CommandRequest& request = {});

private:
    /// A header's self-containment result. NeedsContext carries the
    /// content hash it was scored on — persisted so a stale result is
    /// dropped on cache load; 0 means scored with no disk observation,
    /// which keeps the result session-local.
    struct HeaderResult {
        HeaderMode mode = HeaderMode::Unknown;

        std::uint64_t content_hash = 0;
    };

    /// A header's command borrowed from a host source found through the
    /// include graph, with a synthesized preamble prefix when the header
    /// needs includer context (in `resolution`); nullopt when no usable
    /// host context exists.
    std::optional<CommandRef> resolve_through_host(Fid path_id,
                                                   const CommandRequest& request,
                                                   Resolution& resolution);

    /// The header's context under the request's pinned host while it
    /// still includes the header, else the default host. Fails with the
    /// host whose include chain the synthesis cannot follow under its
    /// command — invalid when no unit includes the header at all.
    std::expected<HeaderContext, Fid> resolve_header_context(Fid header_path_id,
                                                             const CommandRequest& request,
                                                             bool synthesize);

    /// What dump_context_headers would emit for this file (0 = nothing) — the
    /// before/after probe record and reset compare to mark the artifacts
    /// blob dirty exactly when the persisted slice changes. A persisted
    /// result must not outlive its in-memory drop: the header's own
    /// content hash still matches on restart even though a dependency
    /// change deliberately reset the result.
    std::uint64_t persisted_mode_hash(Fid path_id) const;

private:
    /// Self-containment results for headers, persisted in the artifacts
    /// blob. Reset when a closed header or a file along its include chain
    /// changes on disk.
    llvm::DenseMap<Fid, HeaderResult> header_results;

    Project& project;
};

}  // namespace clice
