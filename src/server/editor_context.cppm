module clice:server.editor_context;

import :project.command_resolver;
import :project.project;
import :support.prelude;

namespace clice {

/// The editor's side of command resolution: the user's context choices.
/// Editor-facing compiles resolve through here, layering them over the
/// project's CommandResolver; background compiles never see them. The choices are
/// what the context choices encodes: every change reserializes them into it,
/// and a load parses them back (switchContext waits on its durability).
/// The protocol handlers (clice/queryContext,
/// currentContext, switchContext) live in ContextService and drive this
/// state through its public surface.
struct EditorContext {
    EditorContext(Project& project, CommandResolver& commands, ContextChoices& blob) :
        project(project), commands(commands), blob(blob) {}

    Project& project;
    CommandResolver& commands;
    ContextChoices& blob;

    /// User context choices (clice/switchContext), persisted in the
    /// context choices blob and validated against the CDB and include graph on
    /// didOpen. The single source of truth for a file's active context.
    llvm::DenseMap<Fid, Selection> selections;

    /// The files whose last resolution borrowed or synthesized a command
    /// (Inferred, Fallback). A database change may give one a real command
    /// or change its lender's, which the change's own delta cannot tell;
    /// the invalidator recompiles them on every change.
    llvm::DenseSet<Fid> guessed_commands;

    /// Resolve an open file's command: its pin layered over the project's
    /// resolution.
    Resolution resolve_command(Fid path_id,
                               std::string& directory,
                               std::vector<std::string>& arguments);

    /// The host source of a header: the user's persisted choice, else
    /// clice::host_of.
    Fid host_of(Fid path_id);

    /// The dialect the server lexes an open file's text in, for the results
    /// computed from its index, hover's directive lookup and completion's
    /// context checks: clice::dialect under the file's selection and host.
    const clang::LangOptions& dialect(Fid path_id);

    /// The file's selection, or nullptr.
    const Selection* selection(Fid path_id) const {
        auto it = selections.find(path_id);
        return it != selections.end() ? &it->second : nullptr;
    }

    /// Drop the file's selection, persisting the absence: the choice moved
    /// to another project.
    void forget_selection(Fid path_id) {
        if(selections.erase(path_id)) {
            mark_dirty();
        }
    }

    /// Whether the file's selection still holds against the current CDB
    /// and include graph: its host still compiles and includes the file,
    /// its pinned command still exists.
    bool holds_choice(Fid path_id);

    /// Validate a context choice persisted from an earlier run, dropping it
    /// when it no longer holds (holds_choice). Called on didOpen; a
    /// surviving entry is the file's active context.
    void validate_saved_context(Fid path_id);

    /// Whether a pinned command choice still has a live basis among
    /// `entry_file`'s candidates: its applied hash matches a candidate
    /// under the current edits of `paths` (the host and the header for a
    /// host pin), or its recorded base command hash still names one (a rule
    /// edit moves every applied hash; the base survives it). The validity
    /// test shared by didOpen validation and the server's orphan pass.
    bool pin_alive(Fid entry_file,
                   llvm::ArrayRef<CanonicalRef> paths,
                   const Selection& saved) const;

    /// Mark the choices changed: they reserialize into the blob, the next
    /// save persists them, and a wait on the `save_seq` read now resolves
    /// once it has.
    void mark_dirty();

    /// Restore the choices from the blob as loaded.
    void load();

private:
    std::string serialize() const;
};

}  // namespace clice
