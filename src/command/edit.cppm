module;

#include "support/prelude.h"

module clice:command.edit;

import :command.table;
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

/// Config-rule edits applied on top of a base command (structured, no
/// re-parse), in the order the rules declare them: a remove cancels the
/// matching options of the base command and of the appends before it;
/// appends insert before the input slot so they always take effect for
/// the compile.
struct CommandEdits {
    llvm::ArrayRef<CommandEdit> edits;

    /// Per-run additions in the resolved command's own dialect (a lint
    /// plan's clang-tool extra args). Unlike the config rule lists above
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

    /// The identity of the edits, to memoize apply_edits by: their exact
    /// content and the directory each edit's relative paths are anchored at.
    std::string key() const;
};

/// Apply edits to a base command, producing a (deduped) result command.
/// Structured editing on the arg sequence: remove cancels matching args,
/// appends insert before the input slot. NVCC commands translate the edits
/// into clang spellings first, the way the base command itself was
/// translated, reading the options files they name.
CommandID
    apply_edits(CommandTable& commands, FileTable& files, CommandID id, const CommandEdits& edits);

}  // namespace clice
