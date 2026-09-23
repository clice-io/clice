#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sched/command_resolver.h"
#include "sched/index/store.h"
#include "sched/workspace.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// The editor's side of command resolution: the user's context choices,
/// the header contexts resolved for open files and the hosts of the
/// artifacts synthesized for them. Editor-facing compiles resolve through
/// here, layering this state over the project's CommandResolver;
/// background compiles never see it. Owns the contexts blob, which the
/// index store persists beside the index (switchContext waits on its
/// durability). The protocol handlers (clice/queryContext,
/// currentContext, switchContext) live in ContextService and drive this
/// state through its public surface.
struct EditorContext final : ContextsOwner {
    EditorContext(Workspace& workspace, CommandResolver& commands) :
        workspace(workspace), commands(commands) {}

    Workspace& workspace;
    CommandResolver& commands;

    /// User context choices (clice/switchContext), persisted in the
    /// contexts blob and validated against the CDB and include graph on
    /// didOpen. The single source of truth for a file's active context.
    llvm::DenseMap<Fid, Selection> selections;

    /// Host source of each synthesized artifact (prefix/suffix/snapshot
    /// file path -> host path_id), recorded when an editor resolution
    /// synthesizes it and persisted in the contexts blob. Opening an
    /// artifact compiles it with its host's command — it is a fragment of
    /// that TU, and treated as self-contained (an artifact needing context
    /// itself is out of scope).
    llvm::StringMap<Fid> synthesized_hosts;

    /// Resolved compilation contexts of header files, keyed by the header.
    /// Entries outlive their sessions: closing a header keeps its
    /// synthesized preamble, so reopening reuses it instead of
    /// re-synthesizing. Entries are re-validated at use (deps_changed) and
    /// invalidated by saves along their include chain. An automatic (not
    /// user-chosen) host sticks until such an invalidation — reuse
    /// deliberately wins over re-ranking hosts on reopen.
    /// TODO: entries for headers never reopened accumulate for the server's
    /// lifetime; add eviction if observation shows it matters.
    llvm::DenseMap<Fid, HeaderContext> header_contexts;

    /// The files whose last resolution borrowed or synthesized a command
    /// (Inferred, Fallback). A database change may give one a real command
    /// or change its lender's, which the change's own delta cannot tell;
    /// the invalidator recompiles them on every change.
    llvm::DenseSet<Fid> guessed_commands;

    /// Resolve an open file's command: its pin and cached header context
    /// layered over the project's resolution.
    Resolution resolve_command(llvm::StringRef path,
                               std::string& directory,
                               std::vector<std::string>& arguments);

    /// The file's selection, or nullptr.
    const Selection* selection(Fid path_id) const {
        auto it = selections.find(path_id);
        return it != selections.end() ? &it->second : nullptr;
    }

    /// The file's resolved header context, or nullptr.
    HeaderContext* header_context(Fid path_id) {
        auto it = header_contexts.find(path_id);
        return it != header_contexts.end() ? &it->second : nullptr;
    }

    const HeaderContext* header_context(Fid path_id) const {
        auto it = header_contexts.find(path_id);
        return it != header_contexts.end() ? &it->second : nullptr;
    }

    /// Discard the file's resolved header context so the next compile
    /// re-resolves (and possibly re-synthesizes) it.
    void drop_header_context(Fid path_id) {
        header_contexts.erase(path_id);
    }

    /// The store evicted synthesized files: drop the host records of the
    /// files gone from disk. A resolved context checks its own files on
    /// every reuse, so none needs dropping here.
    void drop_evicted_artifacts();

    /// Drop the header context's dependency fast paths so the next use
    /// re-validates every chain file by a real read. The context itself is
    /// kept: an in-flight compile can clobber ast_dirty when it finishes,
    /// and the surviving snapshot is what lets is_stale() recover. A
    /// self-contained borrow tracks no chain deps, so forcing its
    /// re-validation could never trigger anything — drop it instead and let
    /// the next use re-resolve against the updated include graph (cheap: no
    /// synthesis on that route).
    void invalidate_header_deps(Fid path_id);

    /// Headers whose resolved context embeds `path_id` through its include
    /// chain — the synthesized preamble copies the chain files' content, so
    /// a save along it must force re-validation.
    llvm::SmallVector<Fid> chain_dependents(Fid path_id) const;

    /// Append the header context's suffix as one trailing #include line: the
    /// suffix content (everything after the include position along the chain)
    /// lives in its own file so features never see it, while the token stream
    /// still closes any braces the fragment is embedded in. The single extra
    /// line sits past the editor's EOF and is invisible to the client.
    void append_suffix_include(Fid path_id, std::string& text) const;

    /// Validate a context choice persisted from an earlier run against the
    /// current CDB and include graph, dropping it when stale. Called on
    /// didOpen; a surviving entry is the file's active context.
    void validate_saved_context(Fid path_id);

    /// Whether a pinned command choice still has a live basis among
    /// `entry_file`'s candidates: its applied hash matches a candidate
    /// under the current edits of `paths` (the host and the header for a
    /// host pin), or its recorded base entry hash still names one (a rule
    /// edit moves every applied hash; the base survives it). The validity
    /// test shared by didOpen validation and the server's orphan pass.
    bool pin_alive(Fid entry_file,
                   llvm::ArrayRef<llvm::StringRef> paths,
                   const Selection& saved) const;

    /// Mark the choices changed: the next save persists them, and a
    /// durability ticket taken now resolves once it has.
    void mark_dirty();

    std::string serialize() const override;
    void load(llvm::StringRef bytes) override;

    void rewrite() override {
        mark_dirty();
    }

private:
    /// Record a synthesized artifact's host attribution, marking the
    /// contexts blob dirty when the mapping actually changes — synthesis
    /// re-derives the same content-addressed paths on every resolve, and
    /// an unconditional mark would rewrite the blob each time.
    void record_synthesized_host(llvm::StringRef path, Fid host_path_id);
};

}  // namespace clice
