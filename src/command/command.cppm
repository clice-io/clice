module;

#include "modules/prelude.h"

module clice:command.command;

import :command.argument;
import :command.search_config;
import :support.object_pool;
import :vfs.cygwin;
import :vfs.file_table;

namespace clice {

class Toolchain;

/// A driver-level compile configuration, parsed and classified once at load.
/// Deduped via ObjectSet — identity is the CommandID. Wrapper prefixes
/// (ccache etc.) are entry provenance, not part of the config: `ccache
/// clang++ X` and `clang++ X` dedupe to one config.
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

/// A compile_commands.json's bytes as a JSON reader takes them: it refuses
/// a whole document over one byte that is not UTF-8 (a path or a define in
/// a legacy code page, which CMake writes as it finds them), so each such
/// byte becomes U+FFFD. Nullopt when the bytes are UTF-8.
std::optional<std::string> repaired_database_text(llvm::StringRef bytes);

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
/// config plus the language the toolchain layer probes with. For a header
/// borrowing a host command, `input` is the host's.
struct CommandRef {
    Fid file;
    CommandID command = invalid_command;
    InputKind input;
    CommandSource source = CommandSource::Fallback;
};

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

/// Config-rule edits applied on top of a base config (structured, no
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
};

struct RenderOptions {
    /// Synthesized preamble to inject as `-include <path>`, after the
    /// command's own user-content flags so the host's -include runs first.
    const char* preamble = nullptr;

    /// How many of the command's `-include`s to keep, in order: the ones
    /// past them are left out.
    std::size_t kept_includes = std::numeric_limits<std::size_t>::max();
};

/// A loaded compilation database file. Ids are stable for the database's
/// lifetime; a source reloads or unloads in place.
enum class SourceID : std::uint32_t {};

/// A single entry of one source.
struct CompilationEntry {
    /// Fid of the source file (shared FileTable).
    Fid file;

    CommandID command = invalid_command;

    SourceID source;

    /// Position of the entry in its source file. Generators emit a file's
    /// entries in configuration order, so this is the default-selection
    /// order within one source.
    std::uint32_t ordinal = 0;

    /// How the database names the file when that is not its identity (a
    /// symlinked source), empty otherwise: the build compiles it under that
    /// name, which decides where its quoted includes look and what
    /// `__FILE__` says.
    llvm::StringRef spelling;
};

/// Per-file delta of the build's commands — a compilation database
/// reload's, and the provisional members' and rule sources' that follow
/// it. Path ids are the shared file table's fids (stable across reloads).
struct CDBDiff {
    /// Files present only after the reload (gained their first entry).
    llvm::SmallVector<Fid, 0> added;

    /// Files present only before the reload (lost all their entries).
    llvm::SmallVector<Fid, 0> removed;

    /// Files present on both sides whose sequence of command hashes differs
    /// — a reorder counts, since the first entry is the default selection.
    llvm::SmallVector<Fid, 0> changed;

    bool empty() const {
        return added.empty() && removed.empty() && changed.empty();
    }

    void append(const CDBDiff& other) {
        added.append(other.added);
        removed.append(other.removed);
        changed.append(other.changed);
    }
};

class CompilationDatabase {
public:
    /// The database interns entry files into the workspace-wide table it
    /// is constructed over; entry file ids and workspace fids are the
    /// same ids.
    explicit CompilationDatabase(FileTable& files);
    ~CompilationDatabase();

    CompilationDatabase(const CompilationDatabase&) = delete;
    CompilationDatabase& operator=(const CompilationDatabase&) = delete;

    /// Where probes of cwd-insensitive configs run, and what command hashes
    /// name the paths under relative to (path::portable). Set before load;
    /// empty means the process working directory, and absolute paths.
    void set_workspace_root(CanonicalRef root);

    FileTable& files() {
        return file_table;
    }

    /// Register a database file, or look up its id when already known;
    /// `path` may name a directory holding compile_commands.json. Nothing
    /// is read until load_source().
    SourceID add_source(const Spelling& path);

    std::optional<SourceID> find_source(const Spelling& path) const;

    /// The source registered under another spelling of the file `path`
    /// names: the database a link names, or a link to it — as the root's
    /// compile_commands.json links build/compile_commands.json.
    std::optional<SourceID> find_same_file(const Spelling& path) const;

    llvm::StringRef source_path(SourceID id) const;

    std::size_t source_count() const {
        return source_files.size();
    }

    /// Load (or reload) one source from its file, returning its entry
    /// count. On success the source's old entries are replaced; the pools
    /// and configs survive (path ids stay stable across reloads).
    ///
    /// Parsing is atomic at the top level: if the file cannot be read, is
    /// not valid JSON, has a root that is not an array, or spells POSIX
    /// paths no Cygwin or MSYS2 runtime on PATH maps, the previously loaded
    /// entries are kept and nullopt is returned. Individual
    /// malformed entries are still skipped — which means a file truncated
    /// mid-array loads as a partial set; the poll-side debounce is
    /// what guards against reading half-written files.
    std::optional<std::size_t> load_source(SourceID id);

    /// Drop a source's entries (the source stays registered): the per-file
    /// delta, like a reload's.
    CDBDiff unload_source(SourceID id);

    /// Whether the source's last load succeeded, so its entries are current.
    bool loaded(SourceID id) const;

    /// One file a load read: the database, or a response file its
    /// commands name. `hash` names the bytes the entries were built from;
    /// nullopt when the file could not be read.
    struct LoadInput {
        Fid file;
        std::optional<std::uint64_t> hash;

        friend bool operator==(const LoadInput&, const LoadInput&) = default;
    };

    /// What the source's last load read: the database first, then the
    /// response files (`@file`, nvcc's options files) its commands name,
    /// readable or not — a
    /// change to one changes the commands as much as an edit of the
    /// database itself. A load that could not parse the database read it
    /// alone, its entries staying those of the load before; a source never
    /// loaded has only the database, unread. A watcher compares the disk
    /// against these hashes, not against a stat taken after the load, which
    /// a rewrite arriving in between would already describe.
    llvm::ArrayRef<LoadInput> inputs(SourceID id) const;

    /// Whether the source's file exists on disk as last observed: set by a
    /// successful load, then maintained by the CDBWatcher's stats. A
    /// discovered source that vanished keeps serving its entries but yields
    /// to the present ones (see Build::source_order).
    bool present(SourceID id) const;
    void set_present(SourceID id, bool present);

    /// Register and load `path` in one step; the entry count on success.
    std::optional<std::size_t> load(llvm::StringRef path);

    /// Reload one source and report the per-file delta of the whole
    /// database against its state before the reload.
    ///
    /// Entry identity is the command hash (Frontend view + directory), the
    /// same identity the rest of the system uses to pin a CDB entry (e.g.
    /// clice/switchContext). A change confined to codegen-only flags (-g,
    /// -ffunction-sections, ...) is therefore not reported as `changed` — this is
    /// deliberate. (Optimization level -O* is semantic, not codegen-only: it
    /// defines __OPTIMIZE__, so changing it does count.)
    ///
    /// If the file cannot be read or does not hold a JSON array, the old
    /// entries are kept and nullopt is returned — the caller must retry
    /// rather than treat the failure as "no change".
    std::optional<CDBDiff> reload_and_diff(SourceID id);

    /// All entries for a file across every source, in (source, ordinal)
    /// order — raw storage order; the build view decides the candidate
    /// order. Empty when the file has none.
    llvm::ArrayRef<CompilationEntry> candidate_entries(Fid path_id) const;
    llvm::ArrayRef<CompilationEntry> candidate_entries(llvm::StringRef file);

    bool has_entry(llvm::StringRef file);

    /// All entries, sorted by (file, source, ordinal).
    llvm::ArrayRef<CompilationEntry> entries() const {
        return entry_list;
    }

    const Command& config(CommandID id) const;

    /// Apply config-rule edits to a base config, producing a (deduped)
    /// result config. Structured editing on the arg sequence: remove
    /// cancels matching args, appends insert before the input slot. NVCC
    /// commands translate the edits into clang spellings first, the way
    /// the base command itself was translated. Memoized per
    /// (config, rule set).
    CommandID apply_edits(CommandID id, const CommandEdits& options);

    /// A command written by hand (a rule's default command, the builtin
    /// fallback): normalized once through the entry pipeline with the
    /// input slot synthesized at the end, memoized per (directory,
    /// spelling). `directory` is its working directory. Nullopt (logged
    /// once) when the spelling is not a compile command — blank, or a
    /// launcher with nothing to launch.
    std::optional<CommandID> intern_command(const Spelling& directory,
                                           llvm::ArrayRef<const char*> arguments);

    /// Derive the language of `file` compiled under `id`: walk the
    /// language-selector state machine (-x applies to inputs after it,
    /// -x none resets, /TC and /TP set globally) up to the input slot;
    /// with no governing selector, map the file extension.
    InputKind input_kind(CommandID id, llvm::StringRef file);

    /// The selector-forced language of the config's input, empty when the
    /// extension decides (what input_kind() falls back to).
    llvm::StringRef forced_language(CommandID id) const;

    /// Whether the config's selector forces a C-family language (is_c_family_type).
    bool forces_c_family(CommandID id) const;

    /// The precompiled header the config's command writes: its output, when
    /// it compiles its input as a header (`-x c++-header`) or emits a PCH —
    /// as CMake builds a target's precompiled headers from
    /// `cmake_pch.hxx.cxx`.
    std::optional<Spelling> pch_output(CommandID id) const;

    /// The precompiled headers the config's command may read: its
    /// `-include-pch` file, and beside each header it forces in, the `.pch`
    /// and the `.gch` that clang and GCC load in the header's place.
    llvm::SmallVector<Spelling, 2> pch_inputs(CommandID id) const;

    /// The entries writing `pch` (pch_output), in storage order.
    llvm::ArrayRef<CompilationEntry> pch_writers(Fid pch) const;

    /// Identity hash of a config (Frontend view + slot position + directory
    /// + schema salt), with the directory and the option values under the
    /// workspace root taken by their portable names so a moved checkout
    /// keeps it. The CDB diff identity, the index snapshot command
    /// identity, and — computed over a rules-applied config — the pin
    /// identity of clice/switchContext.
    std::uint64_t command_hash(CommandID id);

    /// command_hash formatted as the persistent/protocol form (16 hex chars).
    std::string command_hash_hex(CommandID id);

    /// Map each file's path_id to the command hashes of its entries in storage
    /// order (a file may own several entries with different flags; the
    /// first is its default selection) — the identity reload_and_diff()
    /// diffs on and the indexer persists to catch command changes across
    /// sessions.
    llvm::DenseMap<Fid, llvm::SmallVector<std::string, 1>> command_hash_snapshot();

    /// Render the full compile argv for a ref: resolve the config through
    /// the toolchain (probe cached; may spawn the driver once per unique
    /// toolchain key) and render at cc1 level; on probe failure degrade to
    /// the driver-level render. Pointers are interned and stable for the
    /// database's lifetime.
    std::vector<const char*> render(const CommandRef& ref, const RenderOptions& opts = {});

    /// Driver-level render only (no toolchain probe): the probe-failure
    /// degradation path, and the base of display forms.
    std::vector<const char*> render_driver(const CommandRef& ref, const RenderOptions& opts = {});

    /// Every argument including codegen and discarded ones: the
    /// debug/display form. No file injection.
    std::vector<const char*> render_full(CommandID id);

    /// Pre-probe the toolchains of the given refs in parallel (deduped by
    /// toolchain key), so later render() calls hit the cache.
    void warm(llvm::ArrayRef<CommandRef> refs);

    /// Header search configuration of a ref, extracted from the resolved
    /// (cc1) config when the toolchain probe succeeds — memoized — else
    /// from the driver-level config.
    SearchConfig search_config(const CommandRef& ref);

    Toolchain& toolchain() {
        return *chain;
    }

    /// Append one command no database file holds, a test's or one given on
    /// the command line, to an anonymous source and return its entry;
    /// nullopt when normalization fails.
    std::optional<CompilationEntry> add_command(llvm::StringRef directory,
                                                llvm::StringRef file,
                                                llvm::ArrayRef<const char*> arguments);

    std::optional<CompilationEntry> add_command(llvm::StringRef directory,
                                                llvm::StringRef file,
                                                llvm::StringRef command);

private:
    friend class Toolchain;

    /// The normalization pipeline (§ wrapper strip → driver info → @rsp
    /// expansion → nvcc translation → parse → classify → path normalize →
    /// dedup). `file` is the entry's normalized path used to pick the input
    /// slot among the command's inputs; invalid synthesizes the slot at the
    /// end.
    std::optional<CommandID> normalize(const Spelling& directory,
                                      Fid file,
                                      llvm::ArrayRef<const char*> arguments);

    std::optional<CommandID> normalize(const Spelling& directory, Fid file, llvm::StringRef command);

    /// Expand @file tokens, and an NVCC command's options files, in place,
    /// driver-mode aware (CL commands tokenize with Windows rules).
    void expand_response_files(llvm::SmallVectorImpl<const char*>& tokens,
                               const Spelling& directory,
                               CompilerFamily family,
                               llvm::StringSaver& saver,
                               unsigned depth = 0);

    /// One parse result of `tokens` as a local arg: a token the table
    /// cannot parse, does not know or reads as an input stays verbatim
    /// (OPT_UNKNOWN, ArgClass::Unknown), a known option keeps its id and
    /// its values, classified.
    LocalArg
        local_arg(const std::expected<kota::option::ParsedArg, kota::option::ParseError>& parsed,
                  llvm::ArrayRef<std::string> tokens);

    /// Dedup the config; its args are persisted only when it is new.
    CommandID save_config(Command config, llvm::ArrayRef<LocalArg> args);

    llvm::ArrayRef<Arg> persist(llvm::ArrayRef<LocalArg> args);

    /// Append the Frontend-view fragments of `id` (the hash input) to out.
    void render_identity(CommandID id, std::string& out);

    /// Rebuild entry_list from every source's entries, sorted by (file,
    /// source, ordinal).
    void rebuild_entry_list();

    std::optional<CompilationEntry> append_test_command(Fid file,
                                                        std::optional<CommandID> normalized);

    std::unique_ptr<llvm::BumpPtrAllocator> allocator = std::make_unique<llvm::BumpPtrAllocator>();

    /// Keep all strings (arguments, directories, etc.).
    llvm::UniqueStringSaver strings{*allocator};

    ObjectSet<Command> configs{allocator.get()};

    /// The process's file table (borrowed by the project that owns this
    /// database — nested databases share one id space).
    FileTable& file_table;

    /// Registered sources: canonical file path and the entries its last
    /// successful load produced (in file order).
    struct Source {
        std::string path;
        std::vector<CompilationEntry> entries;
        bool loaded = false;
        bool present = false;
        std::vector<LoadInput> inputs;
    };

    std::vector<Source> source_files;

    /// The source being loaded, which records the response files its
    /// commands expand among its inputs; nullopt outside a load.
    std::optional<SourceID> loading;

    /// The mount table mapping the POSIX paths of the database entry being
    /// loaded, one written inside Cygwin or MSYS2 (CMake run from their
    /// shell); null outside such an entry.
    const MountTable* mounts = nullptr;

    /// Every source's entries, sorted by (file, source, ordinal).
    std::vector<CompilationEntry> entry_list;

    /// The entries writing each precompiled header (see pch_writers),
    /// rebuilt with entry_list.
    llvm::DenseMap<Fid, llvm::SmallVector<CompilationEntry, 1>> writers;

    /// The path a command gives its input file as: a database's spelling
    /// of it (CompilationEntry::spelling), else its identity.
    llvm::StringRef input_path(Fid file) const;

    CanonicalPath workspace_root;

    /// Derivation memos, append-only alongside the pools.
    llvm::DenseMap<std::uint32_t, std::uint64_t> command_hashes;
    llvm::StringMap<std::uint32_t> rule_set_ids;
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> rule_applied;
    llvm::StringMap<CommandID> interned_commands;
    llvm::DenseMap<std::uint32_t, SearchConfig> search_configs;

    std::unique_ptr<Toolchain> chain;
};

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<clice::Command> {
    using T = clice::Command;

    static unsigned getHashValue(const T& config) {
        llvm::hash_code hash = llvm::hash_combine(config.directory,
                                                  config.driver,
                                                  config.subcommand,
                                                  static_cast<unsigned>(config.family),
                                                  config.query_toolchain);
        for(auto& arg: config.args) {
            hash = llvm::hash_combine(hash,
                                      arg.opt_id,
                                      static_cast<unsigned>(arg.cls),
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
