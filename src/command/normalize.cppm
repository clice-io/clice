module;

#include "support/prelude.h"

module clice:command.normalize;

import :command.argument;
import :command.table;
import :vfs.file_table;
import :vfs.path;

namespace clice {

/// One file a database load read: the database, or a response file its
/// commands name. `hash` names the bytes the entries were built from;
/// nullopt when the file could not be read.
struct LoadInput {
    Fid file;
    std::optional<std::uint64_t> hash;

    friend bool operator==(const LoadInput&, const LoadInput&) = default;
};

/// `text` spelled against `base`, a POSIX path naming what `mounts` maps
/// it to.
Spelling spelled(llvm::StringRef text, const Spelling& base, const fs::cygwin::MountTable* mounts);

/// `value` spelled against `anchor` when it names a path (names_path),
/// else `value` itself.
llvm::StringRef anchored(CommandTable& commands,
                         unsigned id,
                         llvm::StringRef value,
                         const Spelling& anchor,
                         const fs::cygwin::MountTable* mounts = nullptr);

/// Leading tokens forming a compiler-launcher prefix (ccache, distcc, ...,
/// possibly chained), including the wrapper's own leading options. Zero when
/// the command starts with the compiler itself.
std::size_t wrapper_prefix_len(llvm::ArrayRef<const char*> argv);

/// Reads a raw compile command into the table, once: wrapper strip → driver
/// info → @rsp expansion → nvcc translation → parse → classify → path
/// normalize → dedup.
struct Normalizer {
    CommandTable& commands;

    FileTable& files;

    /// The mount table mapping the POSIX paths of the command, one written
    /// inside Cygwin or MSYS2 (CMake run from their shell); null for every
    /// other.
    const fs::cygwin::MountTable* mounts = nullptr;

    /// Where the response files the commands name are recorded, readable
    /// or not; null outside a database load.
    std::vector<LoadInput>* inputs = nullptr;

    /// `file` is the entry's normalized path used to pick the input slot
    /// among the command's inputs; invalid synthesizes the slot at the end.
    /// Nullopt when the arguments are no compile command: empty, or a
    /// launcher with nothing to launch.
    std::optional<CommandID> normalize(const Spelling& directory,
                                       Fid file,
                                       llvm::ArrayRef<const char*> arguments);

    std::optional<CommandID> normalize(const Spelling& directory,
                                       Fid file,
                                       llvm::StringRef command);

    /// Expand @file tokens, and an NVCC command's options files, in place,
    /// driver-mode aware (CL commands tokenize with Windows rules).
    void expand_response_files(llvm::SmallVectorImpl<const char*>& tokens,
                               const Spelling& directory,
                               CompilerFamily family,
                               llvm::StringSaver& saver,
                               unsigned depth = 0);
};

}  // namespace clice
