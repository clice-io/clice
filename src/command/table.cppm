module;

#include "modules/prelude.h"

module clice:command.table;

import :command.argument;
import :support.object_pool;
import :vfs.file_table;
import :vfs.path;

namespace clice {

/// A driver-level compile command, parsed and classified once (Normalizer)
/// and deduped by the CommandTable — identity is the CommandID. Wrapper
/// prefixes (ccache etc.) are entry provenance, not part of the command:
/// `ccache clang++ X` and `clang++ X` dedupe to one command.
struct Command {
    /// Working directory (interned).
    const char* directory = nullptr;

    /// argv[0] as invoked (interned). Never realpath'ed: the invoked name
    /// selects driver behavior (clang vs clang++).
    const char* driver = nullptr;

    /// Zig's subcommand token ("cc" / "c++"); nullptr for everything else.
    const char* subcommand = nullptr;

    /// From the driver filename plus an explicit --driver-mode= scan.
    CompilerFamily family = CompilerFamily::Unknown;

    /// Ordered arguments, exactly one ArgClass::Input slot among them.
    llvm::ArrayRef<Arg> args;

    /// False when a rule turns the toolchain query off: clang's own driver
    /// reads the command, and the compiler never runs.
    bool query_toolchain = true;

    friend bool operator==(const Command&, const Command&) = default;
};

enum class CommandID : std::uint32_t {};

constexpr inline CommandID invalid_command = CommandID(~0u);

/// The language dimension of a command for one input file: the clang
/// language name ("c++", "cuda", ...) selected by the governing selector or
/// derived from the file extension; the raw extension itself when no table
/// maps it. Interned — pointer-comparable.
struct InputKind {
    const char* value = nullptr;

    friend bool operator==(const InputKind&, const InputKind&) = default;
};

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
    /// Synthesized default command — no CDB entry and no usable host source.
    Fallback,
    /// A rule's default_command: declared by the user for files without an
    /// entry, so no guidance note.
    Default,
    /// Borrowed from a unit of the build near the file — a sibling, one
    /// whose header search reaches it, or the closest by path — for a file
    /// with neither an entry nor a host, a provisional member's included
    /// (see command_lender).
    Inferred,
};

/// A resolved command selection for one file: the final (rules-applied)
/// command plus the language the toolchain layer queries with. For a header
/// borrowing a host command, `input` is the host's.
struct CommandRef {
    Fid file;
    CommandID command = invalid_command;
    InputKind input;
    CommandSource source = CommandSource::Fallback;
};

struct RenderOptions {
    /// Synthesized preamble to inject as `-include <path>`, after the
    /// command's own user-content flags so the host's -include runs first.
    const char* preamble = nullptr;

    /// How many of the command's `-include`s to keep, in order: the ones
    /// past them are left out.
    std::size_t kept_includes = std::numeric_limits<std::size_t>::max();
};

/// Get the resource directory for clang builtin headers. Computed once
/// from the current executable path using Driver::GetResourcesPath.
llvm::StringRef resource_dir();

/// Every command clice knows, as the FileTable holds every file: each
/// structured command stored once, its strings interned (pointer-comparable)
/// and kept, together with what derives from one command alone — its
/// language, the precompiled headers it writes and reads, its command hash
/// and its driver-level renders. Append-only: ids and pointers stay valid
/// for the table's lifetime.
class CommandTable {
public:
    CommandTable() = default;

    CommandTable(const CommandTable&) = delete;
    CommandTable& operator=(const CommandTable&) = delete;

    /// Where queries of cwd-insensitive commands run, and what command hashes
    /// name the paths under relative to (path::portable). Set before load;
    /// empty means the process working directory, and absolute paths.
    void set_workspace_root(CanonicalRef path);

    CanonicalRef workspace_root() const {
        return root;
    }

    const Command& get(CommandID id) const;

    /// Dedup the command; its args are persisted only when it is new.
    CommandID save(Command command, llvm::ArrayRef<LocalArg> args);

    llvm::ArrayRef<Arg> persist(llvm::ArrayRef<LocalArg> args);

    llvm::StringRef intern(llvm::StringRef text);

    /// One parse result of `tokens` as a local arg: a token the table
    /// cannot parse, does not know or reads as an input stays verbatim
    /// (OPT_UNKNOWN, ArgClass::Unknown), a known option keeps its id and
    /// its values, classified.
    LocalArg
        local_arg(const std::expected<kota::option::ParsedArg, kota::option::ParseError>& parsed,
                  llvm::ArrayRef<std::string> tokens);

    /// Derive the language of `file` compiled under `id`: walk the
    /// language-selector state machine (-x applies to inputs after it,
    /// -x none resets, /TC and /TP set globally) up to the input slot;
    /// with no governing selector, map the file extension.
    InputKind input_kind(CommandID id, llvm::StringRef file);

    /// The selector-forced language of the command's input, empty when the
    /// extension decides (what input_kind() falls back to).
    llvm::StringRef forced_language(CommandID id) const;

    /// Whether the command's selector forces a C-family language (is_c_family_type).
    bool forces_c_family(CommandID id) const;

    /// The precompiled header the command writes: its output, when it
    /// compiles its input as a header (`-x c++-header`) or emits a PCH —
    /// as CMake builds a target's precompiled headers from
    /// `cmake_pch.hxx.cxx`.
    std::optional<Spelling> pch_output(CommandID id) const;

    /// The precompiled headers the command may read: its `-include-pch`
    /// file, and beside each header it forces in, the `.pch` and the `.gch`
    /// that clang and GCC load in the header's place.
    llvm::SmallVector<Spelling, 2> pch_inputs(CommandID id) const;

    /// Identity hash of a command (Frontend view + slot position + directory
    /// + schema salt), with the directory and the option values under the
    /// workspace root taken by their portable names so a moved checkout
    /// keeps it. The CDB diff identity, the index snapshot command
    /// identity, and — computed over a rules-applied command — the pin
    /// identity of clice/switchContext.
    std::uint64_t command_hash(CommandID id);

    /// command_hash formatted as the persistent/protocol form (16 hex chars).
    std::string command_hash_hex(CommandID id);

    /// Driver-level render (no toolchain query), naming the input `path`:
    /// the query-failure degradation path, and the base of display forms.
    std::vector<const char*> render_driver(const CommandRef& ref,
                                           llvm::StringRef path,
                                           const RenderOptions& opts = {});

    /// Every argument including codegen and discarded ones: the
    /// debug/display form. No file injection.
    std::vector<const char*> render_full(CommandID id);

private:
    /// Append the Frontend-view fragments of `id` (the hash input) to out.
    void render_identity(CommandID id, std::string& out);

private:
    std::unique_ptr<llvm::BumpPtrAllocator> allocator = std::make_unique<llvm::BumpPtrAllocator>();

    /// Keep all strings (arguments, directories, etc.).
    llvm::UniqueStringSaver strings{*allocator};

    ObjectSet<Command> commands{allocator.get()};

    CanonicalPath root;

    llvm::DenseMap<std::uint32_t, std::uint64_t> command_hashes;
};

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<clice::Command> {
    using T = clice::Command;

    static unsigned getHashValue(const T& command) {
        llvm::hash_code hash = llvm::hash_combine(command.directory,
                                                  command.driver,
                                                  command.subcommand,
                                                  static_cast<unsigned>(command.family),
                                                  command.query_toolchain);
        for(auto& arg: command.args) {
            hash = llvm::hash_combine(hash,
                                      arg.opt_id,
                                      static_cast<unsigned>(arg.cls),
                                      arg.alias,
                                      arg.spelling,
                                      llvm::hash_combine_range(arg.values));
        }
        return hash;
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm
