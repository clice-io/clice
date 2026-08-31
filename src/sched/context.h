#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command/command.h"
#include "sched/workspace.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Who a command resolution serves. User context choices steer
/// editor-facing compiles of open files, never background indexing.
enum class ContextUse : std::uint8_t {
    Editor,
    Background,
};

/// Persisted slice entries for context-domain state (the artifacts and
/// contexts blobs in the index database). The structs mirror the on-disk
/// JSON layout field for field — changing them changes the format.

struct CacheModeEntry {
    std::uint32_t file;          // index into the cache path table
    std::uint32_t mode;          // HeaderMode
    std::uint64_t content_hash;  // header contents the verdict was scored on
};

struct CacheContextEntry {
    std::uint32_t file;  // index into the cache path table
    std::uint32_t host;  // index into the cache path table; ~0u = none
    std::uint32_t occurrence;
    std::string command_hash;
    std::string base_hash;
};

struct CacheArtifactEntry {
    std::uint32_t file;  // index into the cache path table
    std::uint32_t host;  // index into the cache path table
};

/// Domain logic for compilation contexts of header files.
///
/// A header without its own compilation database entry borrows a host
/// source's command through the include graph. ContextResolver owns that
/// resolution and synthesis (prefix/suffix/self-snapshot files restoring the
/// includer's preprocessor state) plus the context-domain state: self-
/// containment verdicts, user context choices and synthesized-artifact
/// attribution (all persisted through the index database), and the resolved header
/// contexts, which outlive their sessions so a reopened header reuses its
/// synthesized preamble. The editor-facing context protocol handlers
/// (clice/queryContext, currentContext, switchContext) live on the server
/// side and drive this state through its public surface.
class ContextResolver {
public:
    explicit ContextResolver(Workspace& workspace) : workspace(workspace) {}

    /// Self-containment verdicts for headers, persisted in the artifacts
    /// blob. Reset when the header itself is saved.
    llvm::DenseMap<std::uint32_t, HeaderMode> header_modes;

    /// Content hash of the header at the time its NeedsContext verdict was
    /// scored — persisted so a stale verdict is dropped on cache load.
    llvm::DenseMap<std::uint32_t, std::uint64_t> header_mode_hashes;

    /// User context choices (clice/switchContext), persisted in the contexts blob
    /// and validated against the CDB and include graph on didOpen. The
    /// single source of truth for a file's active context.
    llvm::DenseMap<std::uint32_t, SavedContext> saved_contexts;

    /// Host source of each synthesized artifact (prefix/suffix/snapshot
    /// file path -> host path_id), recorded at synthesis time and
    /// persisted in the contexts blob. Opening an artifact compiles it with its
    /// host's command — it is a fragment of that TU, and treated as
    /// self-contained (an artifact needing context itself is out of scope).
    llvm::StringMap<std::uint32_t> synthesized_hosts;

    /// Resolved compilation contexts of header files, keyed by the header.
    /// Entries outlive their sessions: closing a header keeps its
    /// synthesized preamble, so reopening reuses it instead of
    /// re-synthesizing. Entries are re-validated at use (deps_changed) and
    /// invalidated by saves along their include chain. An automatic (not
    /// user-chosen) host sticks until such an invalidation — reuse
    /// deliberately wins over re-ranking hosts on reopen.
    /// TODO: entries for headers never reopened accumulate for the server's
    /// lifetime; add eviction if observation shows it matters.
    llvm::DenseMap<std::uint32_t, HeaderContext> header_contexts;

    /// The file's resolved header context, or nullptr.
    HeaderContext* header_context(std::uint32_t path_id) {
        auto it = header_contexts.find(path_id);
        return it != header_contexts.end() ? &it->second : nullptr;
    }

    const HeaderContext* header_context(std::uint32_t path_id) const {
        auto it = header_contexts.find(path_id);
        return it != header_contexts.end() ? &it->second : nullptr;
    }

    /// Discard the file's resolved header context so the next compile
    /// re-resolves (and possibly re-synthesizes) it.
    void drop_header_context(std::uint32_t path_id) {
        header_contexts.erase(path_id);
    }

    /// Drop the header context's dependency fast paths so the next use
    /// re-validates every chain file by a real read. The context itself is
    /// kept: an in-flight compile can clobber ast_dirty when it finishes,
    /// and the surviving snapshot is what lets is_stale() recover. A
    /// self-contained borrow tracks no chain deps, so forcing its
    /// re-validation could never trigger anything — drop it instead and let
    /// the next use re-resolve against the updated include graph (cheap: no
    /// synthesis on that route).
    void invalidate_header_deps(std::uint32_t path_id) {
        auto* context = header_context(path_id);
        if(!context) {
            return;
        }
        if(context->deps.empty()) {
            drop_header_context(path_id);
        } else {
            context->deps.force_revalidate(workspace.file_table);
        }
    }

    /// Headers whose resolved context embeds `path_id` through its include
    /// chain — the synthesized preamble copies the chain files' content, so
    /// a save along it must force re-validation.
    llvm::SmallVector<std::uint32_t> chain_dependents(std::uint32_t path_id) const {
        llvm::SmallVector<std::uint32_t> result;
        for(auto& [header_id, context]: header_contexts) {
            if(llvm::is_contained(context.chain, path_id)) {
                result.push_back(header_id);
            }
        }
        return result;
    }

    /// Effective self-containment mode for a header. X-macro style
    /// extensions are non-self-contained by construction; otherwise use
    /// the persisted verdict. Only NeedsContext is ever persisted — a
    /// "self-contained" impression is session-local and re-evaluated when
    /// compile inputs change, so it can never go stale.
    HeaderMode header_mode(llvm::StringRef path, std::uint32_t path_id) const;

    /// Drop an in-memory SelfContained verdict (never a persisted
    /// NeedsContext) so the next compile re-runs the trial.
    void forget_self_contained(std::uint32_t path_id);

    /// Record a header trial's verdict. NeedsContext carries the content
    /// hash it was scored on so a stale verdict is dropped on cache load;
    /// scored with no disk observation (hash 0) it stays session-local.
    /// Marks the artifacts blob dirty when the persisted slice changes.
    void record_header_mode(std::uint32_t path_id, HeaderMode mode, std::uint64_t content_hash = 0);

    /// Drop a header's verdict entirely (its content changed); the next
    /// compile re-earns it. Marks the artifacts blob dirty when a
    /// persisted verdict is dropped.
    void reset_header_mode(std::uint32_t path_id);

    /// Fill the validation slice of the artifacts blob (header-mode
    /// verdicts, content-hash gated at load). @param intern_id maps a
    /// runtime path id into the blob's path table; interning order is part
    /// of the on-disk format.
    void dump_mode_slices(std::vector<CacheModeEntry>& modes,
                          llvm::function_ref<std::uint32_t(std::uint32_t)> intern_id) const;

    /// Restore header-mode verdicts. @param resolve maps a path table
    /// index back to a path (empty when the index is invalid).
    void load_mode_slices(llvm::ArrayRef<CacheModeEntry> modes,
                          llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve);

    /// Fill the sovereignty slices of the contexts blob (user context
    /// choices and synthesized-artifact hosts — never invalidated by
    /// content, only by the user or a vanished CDB anchor). @param
    /// intern_path interns a raw path (synthesized artifacts have no fid
    /// requirement at this layer).
    void dump_choice_slices(std::vector<CacheContextEntry>& contexts,
                            std::vector<CacheArtifactEntry>& artifacts,
                            llvm::function_ref<std::uint32_t(std::uint32_t)> intern_id,
                            llvm::function_ref<std::uint32_t(llvm::StringRef)> intern_path) const;

    /// Restore the sovereignty slices; see dump_choice_slices.
    void load_choice_slices(llvm::ArrayRef<CacheContextEntry> contexts,
                            llvm::ArrayRef<CacheArtifactEntry> artifacts,
                            llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve);

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: CDB entry, header context through the include graph,
    /// and finally a synthesized fallback command — so it always succeeds.
    /// Emits a per-file decision log (tiers tried, tier hit, command hash).
    /// @param host_path_id  If non-null, receives the host source whose
    /// command an IncludeGraph resolution borrowed (untouched otherwise).
    /// @param extra_prepend / extra_append  Per-run additions to the CDB
    /// driver command (a lint plan's clang-tool extra args), applied
    /// before toolchain resolution so the driver interprets them. Unlike
    /// config rule appends they are never NVCC-translated; prepends land
    /// right after the binary name, appends win over rule appends.
    /// @param out_ref  If non-null, receives the resolved command selection
    /// (the ref behind `arguments`) for structured consumers — search
    /// config, language queries.
    CommandSource resolve_command(llvm::StringRef path,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  ContextUse use = ContextUse::Background,
                                  std::uint32_t* host_path_id = nullptr,
                                  llvm::ArrayRef<std::string> extra_prepend = {},
                                  llvm::ArrayRef<std::string> extra_append = {},
                                  CommandRef* out_ref = nullptr);

    /// Append the header context's suffix as one trailing #include line: the
    /// suffix content (everything after the include position along the chain)
    /// lives in its own file so features never see it, while the token stream
    /// still closes any braces the fragment is embedded in. The single extra
    /// line sits past the editor's EOF and is invisible to the client.
    void append_suffix_include(std::uint32_t path_id, std::string& text);

    /// Fill compile arguments for a header from a host source's command found
    /// through the include graph, synthesizing a preamble prefix/suffix when
    /// the header needs includer context. Returns false when no usable host
    /// context exists.
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  ContextUse use,
                                  std::uint32_t* host_path_id,
                                  CommandRef* out_ref = nullptr);

    /// Validate a context choice persisted from an earlier run against the
    /// current CDB and include graph, dropping it when stale. Called on
    /// didOpen; a surviving entry is the file's active context.
    void validate_saved_context(std::uint32_t path_id);

    /// The file's context choice, or nullptr — editor use only: user
    /// choices steer editor-facing compiles, never background indexing.
    const SavedContext* active_choice(ContextUse use, std::uint32_t path_id) const {
        if(use != ContextUse::Editor) {
            return nullptr;
        }
        auto it = saved_contexts.find(path_id);
        return it != saved_contexts.end() ? &it->second : nullptr;
    }

    /// Whether a pinned command choice still has a live basis among
    /// `entry_path`'s CDB entries: its applied hash matches a candidate
    /// under current rules, or its recorded base entry hash still names
    /// one (a rule edit moves every applied hash; the base survives it).
    /// The validity test shared by didOpen validation and the server's
    /// orphan pass.
    bool pin_alive(llvm::StringRef entry_path, const SavedContext& saved) const;

private:
    std::optional<HeaderContext> resolve_header_context(std::uint32_t header_path_id,
                                                        ContextUse use,
                                                        bool synthesize);

    /// What dump_mode_slices would emit for this file (0 = nothing) — the
    /// before/after probe record and reset compare to mark the artifacts
    /// blob dirty exactly when the persisted slice changes. A persisted
    /// verdict must not outlive its in-memory drop: the header's own
    /// content hash still matches on restart even though a dependency
    /// change deliberately reset the verdict.
    std::uint64_t persisted_mode_hash(std::uint32_t path_id) const;

    Workspace& workspace;
};

}  // namespace clice
