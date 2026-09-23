#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "command/command.h"
#include "project/project.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A persisted header-mode verdict in the artifacts blob. Mirrors the
/// on-disk JSON layout field for field — changing it changes the format.
struct CacheModeEntry {
    std::uint32_t file;          // index into the cache path table
    std::uint32_t mode;          // HeaderMode
    std::uint64_t content_hash;  // header contents the verdict was scored on
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

    /// Hosts of synthesized artifacts: an opened artifact compiles under
    /// its host's command.
    const llvm::StringMap<Fid>* synthesized_hosts = nullptr;
};

/// How a file's command was resolved.
struct Resolution {
    CommandSource source = CommandSource::Fallback;

    /// The host whose command an IncludeGraph resolution borrowed;
    /// invalid for every other source.
    Fid host;

    /// The command selection behind the rendered arguments, for
    /// structured consumers (search config, language queries).
    CommandRef ref;

    /// Files this resolution synthesized for the header's context
    /// (preamble, suffix, self snapshot): fragments of `host`'s TU.
    llvm::SmallVector<std::string, 3> synthesized;
};

/// Composes a file's final compile command from the project on disk.
///
/// The build (`Build`) says what a file compiles as; the hosting layer
/// (`default_host`) says which unit stands in for a header without a
/// command of its own. `resolve_command` applies them in order — own
/// entry, host, default command, lender, builtin — then the rule edits
/// and a run's extras; an editor request layers the user's pin and its
/// cached header contexts on top. Around the host branch it owns the
/// header-context synthesis (prefix/suffix/self-snapshot files restoring
/// the includer's preprocessor state, content-addressed in the cache
/// store) and the self-containment verdicts that decide it, persisted in
/// the artifacts blob.
class CommandResolver {
public:
    explicit CommandResolver(Project& project) : project(project) {}

    /// A header's self-containment verdict. NeedsContext carries the
    /// content hash it was scored on — persisted so a stale verdict is
    /// dropped on cache load; 0 means scored with no disk observation,
    /// which keeps the verdict session-local.
    struct HeaderVerdict {
        HeaderMode mode = HeaderMode::Unknown;
        std::uint64_t content_hash = 0;
    };

    /// Self-containment verdicts for headers, persisted in the artifacts
    /// blob. Reset when the header itself is saved.
    llvm::DenseMap<Fid, HeaderVerdict> header_verdicts;

    /// Effective self-containment mode for a header. X-macro style
    /// extensions are non-self-contained by construction; otherwise use
    /// the persisted verdict. Only NeedsContext is ever persisted — a
    /// "self-contained" impression is session-local and re-evaluated when
    /// compile inputs change, so it can never go stale.
    HeaderMode header_mode(llvm::StringRef path, Fid path_id) const;

    /// Drop an in-memory SelfContained verdict (never a persisted
    /// NeedsContext) so the next compile re-runs the trial.
    void forget_self_contained(Fid path_id);

    /// Record a header trial's verdict. NeedsContext carries the content
    /// hash it was scored on so a stale verdict is dropped on cache load;
    /// scored with no disk observation (hash 0) it stays session-local.
    /// Marks the artifacts blob dirty when the persisted slice changes.
    void record_header_mode(Fid path_id, HeaderMode mode, std::uint64_t content_hash = 0);

    /// Drop a header's verdict entirely (its content changed); the next
    /// compile re-earns it. Marks the artifacts blob dirty when a
    /// persisted verdict is dropped.
    void reset_header_mode(Fid path_id);

    /// Fill the validation slice of the artifacts blob (header-mode
    /// verdicts, content-hash gated at load). @param intern_id maps a
    /// runtime path id into the blob's path table; interning order is part
    /// of the on-disk format.
    void dump_mode_slices(std::vector<CacheModeEntry>& modes,
                          llvm::function_ref<std::uint32_t(Fid)> intern_id) const;

    /// Restore header-mode verdicts. @param resolve maps a path table
    /// index back to a path (empty when the index is invalid).
    void load_mode_slices(llvm::ArrayRef<CacheModeEntry> modes,
                          llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve);

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: the pinned host, the file's own command, a header
    /// context through the include graph, a default command, a lender and
    /// finally the builtin command — so it always succeeds. Emits a
    /// per-file decision log (tiers tried, tier hit, command hash).
    Resolution resolve_command(llvm::StringRef path,
                               std::string& directory,
                               std::vector<std::string>& arguments,
                               const CommandRequest& request = {});

private:
    /// Fill compile arguments for a header from a host source's command found
    /// through the include graph, synthesizing a preamble prefix/suffix when
    /// the header needs includer context. Returns false when no usable host
    /// context exists.
    bool fill_header_context_args(llvm::StringRef path,
                                  Fid path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  const CommandRequest& request,
                                  Resolution& resolution);

    std::optional<HeaderContext>
        resolve_header_context(Fid header_path_id,
                               const Selection* choice,
                               bool synthesize,
                               llvm::SmallVectorImpl<std::string>& synthesized_files);

    /// What dump_mode_slices would emit for this file (0 = nothing) — the
    /// before/after probe record and reset compare to mark the artifacts
    /// blob dirty exactly when the persisted slice changes. A persisted
    /// verdict must not outlive its in-memory drop: the header's own
    /// content hash still matches on restart even though a dependency
    /// change deliberately reset the verdict.
    std::uint64_t persisted_mode_hash(Fid path_id) const;

    Project& project;
};

}  // namespace clice
