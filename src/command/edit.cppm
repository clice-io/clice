module clice:command.edit;

import :command.table;
import :support.prelude;
import :vfs.file_table;
import :vfs.path;

namespace clice {

/// One rule's edit of a command: flags to remove from it, or flags to
/// append to it.
struct CommandEdit {
    enum class Kind : std::uint8_t {
        Remove,
        Append,
    };

    Kind kind;
    std::vector<std::string> flags;

    /// Where the rule's relative paths are relative to: the project root,
    /// whichever file the rule is written in.
    Spelling directory;
};

/// A run's own edits of the commands it compiles (a lint plan's clang-tidy
/// ExtraArgsBefore, ExtraArgs and RemovedArgs), in the resolved command's
/// own dialect: unlike a rule's they are never NVCC-translated — clang-tidy's
/// args are clang args by definition. Prepends land right after the binary
/// name, ahead of the command's own flags (the command wins on collision);
/// removes cancel the matching options of the command the rules edited;
/// appends land after the rule appends and win.
struct ExtraArgs {
    llvm::ArrayRef<std::string> prepend;
    llvm::ArrayRef<std::string> append;
    llvm::ArrayRef<std::string> remove;

    bool empty() const {
        return prepend.empty() && append.empty() && remove.empty();
    }
};

/// Rule edits applied on top of a base command (structured, no
/// re-parse), in the order the rules declare them: a remove cancels the
/// matching options of the base command and of the appends before it;
/// appends insert before the input slot so they always take effect for
/// the compile.
struct CommandEdits {
    llvm::ArrayRef<CommandEdit> edits;

    /// The run's own, after the rules'.
    ExtraArgs extra;

    /// False when a rule turns the toolchain query off.
    bool query_toolchain = true;

    bool empty() const {
        return edits.empty() && extra.empty() && query_toolchain;
    }

    /// The identity of the edits: their content and the directory each
    /// edit's relative paths are anchored at.
    std::string key() const;
};

/// Apply edits to a base command, producing a (deduped) result command.
/// Structured editing on the arg sequence: remove cancels matching args,
/// appends insert before the input slot. NVCC commands translate the edits
/// into clang spellings first, the way the base command itself was
/// translated, reading the options files they name.
CommandID
    apply_edits(CommandTable& commands, FileTable& files, CommandID id, const CommandEdits& edits);

/// apply_edits, memoized by the command and the edits.
class EditedCommands {
public:
    EditedCommands(CommandTable& commands, FileTable& files) : commands(commands), files(files) {}

    CommandID apply(CommandID command, const CommandEdits& edits);

private:
    CommandTable& commands;
    FileTable& files;

    /// The ids of the edits' keys apply has seen.
    llvm::StringMap<std::uint32_t> keys;

    /// apply's results by (command, the id of the edits' key).
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, CommandID> edited;
};

}  // namespace clice
