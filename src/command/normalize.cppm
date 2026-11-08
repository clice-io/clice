module clice:command.normalize;

import :command.argument;
import :command.table;
import :support.prelude;
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
std::size_t wrapper_prefix_len(kotatsu::string_list_ref argv);

/// What the commands of entries a build compiles alike share
/// (Normalizer::shared_key): a 128-bit hash.
using SharedKey = std::pair<std::uint64_t, std::uint64_t>;

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
    /// Nullopt when `command_line` is no compile command: empty, naming an
    /// empty program, or a launcher with nothing to launch.
    std::optional<CommandID> normalize(const Spelling& directory,
                                       Fid file,
                                       kotatsu::string_list_ref command_line);

    std::optional<CommandID> normalize(const Spelling& directory,
                                       Fid file,
                                       llvm::StringRef command);

    /// The tokens of `command`, quoted as the database writes it: views of
    /// it in `views` while none needs unquoting, else every one in `tokens`,
    /// whose strings keep their storage for the next command.
    kotatsu::string_list_ref split(llvm::StringRef command,
                                   llvm::SmallVectorImpl<std::string_view>& views,
                                   std::vector<std::string>& tokens) const;

    /// What the commands of entries a build compiles alike share: a hash of
    /// `command_line` run from `directory`, with the entry's `file` as the
    /// database writes it and the values every command leaves out (its
    /// output, its dependency files, `-frandom-seed`) masked, so a database
    /// normalizes each such command once. None when the command names its
    /// file other than once, or reads files of its own (a response file,
    /// nvcc's options files), which a normalization records.
    std::optional<SharedKey> shared_key(const Spelling& directory,
                                        llvm::StringRef file,
                                        kotatsu::string_list_ref command_line) const;

    /// `arguments` with their response files read, and an NVCC command's
    /// options files, driver-mode aware: CL commands tokenize with Windows
    /// rules regardless of the server platform. Relative names resolve
    /// against `directory`; contents may nest.
    std::vector<std::string> expand_response_files(kotatsu::string_list_ref arguments,
                                                   const Spelling& directory,
                                                   CompilerFamily family,
                                                   unsigned depth = 0);
};

/// The commands written by hand — a rule's default command, the builtin
/// fallback — interned: each normalized once through the entry pipeline
/// with the input slot synthesized at the end, by (directory, spelling).
class WrittenCommands {
public:
    WrittenCommands(CommandTable& commands, FileTable& files) : commands(commands), files(files) {}

    /// The command `arguments` spell, run from `directory`; nullopt (logged
    /// once) when it is no compile command — blank, naming an empty
    /// program, or a launcher with nothing to launch.
    std::optional<CommandID> intern(const Spelling& directory, kotatsu::string_list_ref arguments);

private:
    CommandTable& commands;
    FileTable& files;

    /// The commands by (directory, spelling), invalid_command for a
    /// spelling that is not a compile command.
    llvm::StringMap<CommandID> interned;
};

}  // namespace clice
