#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command/command.h"
#include "server/context/context_cache.h"
#include "server/protocol/extension.h"
#include "server/session/session.h"
#include "server/workspace/workspace.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct SessionStore;

namespace protocol = kota::ipc::protocol;

/// Where the compile command for a file came from. Anything other than
/// CDBExact means the command was guessed to some degree, which is why
/// diagnostics produced with it may deserve a guidance note (see
/// format_diagnostics).
enum class CommandSource : std::uint8_t {
    /// Direct compilation database entry for the file.
    CDBExact,
    /// Header compiled in the context of a host source found through the
    /// include graph (automatic or via clice/switchContext).
    IncludeGraph,
    /// Reserved for command transfer heuristics (e.g. nearest CDB entry);
    /// no producer yet.
    Inferred,
    /// Synthesized default command — no CDB entry and no usable host source.
    Fallback,
};

/// Diagnostic codes that strictly indicate a missing includer context (as
/// opposed to ordinary in-progress typing errors). Deliberately narrow:
/// a false positive costs a pointless prefix synthesis, a false negative
/// just leaves the header in trial mode.
bool indicates_missing_context(llvm::ArrayRef<protocol::Diagnostic> diagnostics);

/// Domain logic for compilation contexts of header files.
///
/// A header without its own compilation database entry borrows a host
/// source's command through the include graph. ContextResolver owns that
/// resolution and synthesis (prefix/suffix/self-snapshot files restoring the
/// includer's preprocessor state), the editor-facing context protocol
/// extension (clice/queryContext, currentContext, switchContext), and
/// validating a persisted context choice on didOpen.
///
/// Owns the context-domain state: self-containment verdicts, user context
/// choices and synthesized-artifact attribution (all persisted in
/// cache.json), plus the resolved header contexts, which outlive their
/// sessions so a reopened header reuses its synthesized preamble. Used by
/// Compiler (compile-argument resolution) and LSPClient (protocol handlers)
/// through a shared reference.
class ContextResolver {
public:
    explicit ContextResolver(Workspace& workspace) : workspace(workspace) {}

    /// Self-containment verdicts for headers, persisted in cache.json.
    /// Reset when the header itself is saved.
    llvm::DenseMap<std::uint32_t, HeaderMode> header_modes;

    /// Content hash of the header at the time its NeedsContext verdict was
    /// scored — persisted so a stale verdict is dropped on cache load.
    llvm::DenseMap<std::uint32_t, std::uint64_t> header_mode_hashes;

    /// User context choices (clice/switchContext), persisted in cache.json
    /// and validated against the CDB and include graph on didOpen. The
    /// single source of truth for a file's active context.
    llvm::DenseMap<std::uint32_t, SavedContext> saved_contexts;

    /// Host source of each synthesized artifact (prefix/suffix/snapshot
    /// file path -> host path_id), recorded at synthesis time and
    /// persisted in cache.json. Opening an artifact compiles it with its
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

    /// Zero the header context's dependency baseline so the next use
    /// re-validates every chain file by content hash. The context itself is
    /// kept: an in-flight compile can clobber ast_dirty when it finishes,
    /// and the surviving snapshot is what lets is_stale() recover. A
    /// self-contained borrow tracks no chain deps, so zeroing its baseline
    /// could never force anything — drop it instead and let the next use
    /// re-resolve against the updated include graph (cheap: no synthesis
    /// on that route).
    void invalidate_header_deps(std::uint32_t path_id) {
        auto* context = header_context(path_id);
        if(!context) {
            return;
        }
        if(context->deps.path_ids.empty()) {
            drop_header_context(path_id);
        } else {
            context->deps.build_at = 0;
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
    /// hash it was scored on so a stale verdict is dropped on cache load.
    void record_header_mode(std::uint32_t path_id, HeaderMode mode, std::uint64_t content_hash = 0);

    /// Drop a header's verdict entirely (its content changed); the next
    /// compile re-earns it.
    void reset_header_mode(std::uint32_t path_id);

    /// Fill the context-domain cache.json slices. @param intern_id maps a
    /// runtime path id and @param intern_path a raw path into the shared
    /// cache path table; interning order is part of the on-disk format.
    void dump_cache_slices(std::vector<CacheModeEntry>& modes,
                           std::vector<CacheContextEntry>& contexts,
                           std::vector<CacheArtifactEntry>& artifacts,
                           llvm::function_ref<std::uint32_t(std::uint32_t)> intern_id,
                           llvm::function_ref<std::uint32_t(llvm::StringRef)> intern_path) const;

    /// Restore the context-domain state from cache.json slices. @param
    /// resolve maps a cache path table index back to a path (empty when the
    /// index is invalid).
    void load_cache_slices(const std::vector<CacheModeEntry>& modes,
                           const std::vector<CacheContextEntry>& contexts,
                           const std::vector<CacheArtifactEntry>& artifacts,
                           llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve);

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: CDB entry, header context through the include graph,
    /// and finally a synthesized fallback command — so it always succeeds.
    /// Emits a per-file decision log (tiers tried, tier hit, command hash).
    /// @param session  If non-null, used for header context resolution on open files.
    CommandSource resolve_command(llvm::StringRef path,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  Session* session = nullptr);

    /// Append the header context's suffix as one trailing #include line: the
    /// suffix content (everything after the include position along the chain)
    /// lives in its own file so features never see it, while the token stream
    /// still closes any braces the fragment is embedded in. The single extra
    /// line sits past the editor's EOF and is invisible to the client.
    void append_suffix_include(const Session& session, std::string& text);

    /// Fill compile arguments for a header from a host source's command found
    /// through the include graph, synthesizing a preamble prefix/suffix when
    /// the header needs includer context. Returns false when no usable host
    /// context exists. @param session may be null (background indexing).
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  Session* session);

    /// Validate a context choice persisted from an earlier run against the
    /// current CDB and include graph, dropping it when stale. Called on
    /// didOpen; a surviving entry is the file's active context.
    void validate_saved_context(Session& session);

    /// Drop active context choices whose include edge no longer exists. A
    /// stale choice suppresses automatic host resolution, so it would strand
    /// the header on the fallback command (or silently pin its command hash
    /// to a different host). Expects the include graph to be current (the
    /// caller rescans on save). Returns whether any persisted choice was
    /// removed, i.e. whether the cache snapshot needs saving.
    bool drop_orphaned_choices(SessionStore& sessions);

    /// clice/queryContext: list the compilation contexts (host sources and
    /// the file's own CDB configurations) available for a file, paginated.
    ext::QueryContextResult query_contexts(llvm::StringRef path,
                                           std::uint32_t path_id,
                                           const ext::QueryContextParams& params);

    /// clice/currentContext: describe the file's currently active context.
    ext::CurrentContextResult current_context(llvm::StringRef path,
                                              const Session* session,
                                              const ext::CurrentContextParams& params);

    /// clice/switchContext: pin a host source or CDB entry as the file's
    /// compilation context and persist the choice across sessions.
    ext::SwitchContextResult switch_context(llvm::StringRef path,
                                            std::uint32_t path_id,
                                            Session* session,
                                            llvm::StringRef context_path,
                                            std::uint32_t context_path_id,
                                            const ext::SwitchContextParams& params);

private:
    std::optional<HeaderContext> resolve_header_context(std::uint32_t header_path_id,
                                                        Session* session,
                                                        bool synthesize);

    /// Whether the CDB still holds an entry for `entry_path` whose canonical
    /// command hash equals `hash` — the validity test for a pinned context
    /// choice, shared by didOpen validation and the runtime orphan pass.
    bool entry_has_hash(llvm::StringRef entry_path, llvm::StringRef hash) const;

    /// The file's context choice, or nullptr. Gated on an open session:
    /// user choices steer editor-facing compiles, never background indexing
    /// (which passes no session).
    const SavedContext* active_choice(const Session* session, std::uint32_t path_id) const {
        if(!session) {
            return nullptr;
        }
        auto it = saved_contexts.find(path_id);
        return it != saved_contexts.end() ? &it->second : nullptr;
    }

    Workspace& workspace;
};

}  // namespace clice
