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

    /// Where the rule's relative paths are relative to: its configuration
    /// file's directory.
    Spelling directory;
};

/// Rule edits applied on top of a base command (structured, no
/// re-parse), in the order the rules declare them: a remove cancels the
/// matching options of the base command and of the appends before it;
/// appends insert before the input slot so they always take effect for
/// the compile.
struct CommandEdits {
    llvm::ArrayRef<CommandEdit> edits;

    /// Per-run additions in the resolved command's own dialect (a lint
    /// plan's clang-tool extra args). Unlike the rule lists above
    /// they are never NVCC-translated — clang-tidy's extra args are clang
    /// args by definition. Prepends land right after the binary name,
    /// ahead of the command's own flags (the command wins on collision);
    /// appends land after the rule appends and win.
    llvm::ArrayRef<std::string> extra_prepend;
    llvm::ArrayRef<std::string> extra_append;

    /// False when a rule turns the toolchain query off.
    bool query_toolchain = true;

    bool empty() const {
        return edits.empty() && extra_prepend.empty() && extra_append.empty() && query_toolchain;
    }

    /// The identity of the edits: their content and the directory each
    /// edit's relative paths are anchored at, `root` taken as `${workspace}`
    /// — so a moved checkout keeps the identity a record persists.
    std::string key(llvm::StringRef root = {}) const;
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
