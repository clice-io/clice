#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project/command_resolver.h"
#include "project/index_store.h"
#include "project/project.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// The editor's side of command resolution: the user's context choices and
/// the header contexts resolved for open files. Editor-facing compiles
/// resolve through here, layering this state over the project's
/// CommandResolver; background compiles never see it. The choices are
/// what the contexts blob encodes: every change reserializes them into it,
/// and a load parses them back (switchContext waits on its durability).
/// The protocol handlers (clice/queryContext,
/// currentContext, switchContext) live in ContextService and drive this
/// state through its public surface.
struct EditorContext {
    EditorContext(Project& project, CommandResolver& commands, ContextsBlob& blob) :
        project(project), commands(commands), blob(blob) {}

    Project& project;
    CommandResolver& commands;
    ContextsBlob& blob;

    /// User context choices (clice/switchContext), persisted in the
    /// contexts blob and validated against the CDB and include graph on
    /// didOpen. The single source of truth for a file's active context.
    llvm::DenseMap<Fid, Selection> selections;

    /// Resolved compilation contexts of header files, keyed by the header.
    /// Entries outlive their sessions: closing a header keeps its
    /// synthesized context, so reopening reuses it instead of
    /// re-synthesizing. Entries are re-validated at use (deps_changed) and
    /// dropped when a file along their include chain changes on disk. An
    /// automatic (not
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

    /// Drop the file's selection and the header context resolved under it,
    /// persisting the absence: the choice moved to another project.
    void forget_selection(Fid path_id) {
        if(selections.erase(path_id)) {
            drop_header_context(path_id);
            mark_dirty();
        }
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

    /// Headers whose resolved context was derived through `path_id` — a
    /// file along its include chain.
    llvm::SmallVector<Fid> chain_dependents(Fid path_id) const;

    /// Whether the file's selection still holds against the current CDB
    /// and include graph: its host still compiles and includes the file,
    /// its pinned command still exists.
    bool holds_choice(Fid path_id) const;

    /// Validate a context choice persisted from an earlier run, dropping it
    /// when it no longer holds (holds_choice). Called on didOpen; a
    /// surviving entry is the file's active context.
    void validate_saved_context(Fid path_id);

    /// Whether a pinned command choice still has a live basis among
    /// `entry_file`'s candidates: its applied hash matches a candidate
    /// under the current edits of `paths` (the host and the header for a
    /// host pin), or its recorded base entry hash still names one (a rule
    /// edit moves every applied hash; the base survives it). The validity
    /// test shared by didOpen validation and the server's orphan pass.
    bool pin_alive(Fid entry_file,
                   llvm::ArrayRef<CanonicalRef> paths,
                   const Selection& saved) const;

    /// Mark the choices changed: they reserialize into the blob, the next
    /// save persists them, and a durability ticket taken now resolves once
    /// it has.
    void mark_dirty();

    /// Restore the choices from the blob as loaded.
    void load();

private:
    std::string serialize() const;
};

}  // namespace clice
