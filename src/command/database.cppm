module clice:command.database;

import :command.normalize;
import :command.table;
import :support.prelude;
import :vfs.file_table;
import :vfs.path;

namespace clice {

/// A compile_commands.json's bytes as a JSON reader takes them: it refuses
/// a whole document over one byte that is not UTF-8 (a path or a define in
/// a legacy code page, which CMake writes as it finds them), so each such
/// byte becomes U+FFFD. Nullopt when the bytes are UTF-8.
std::optional<std::string> repaired_database_text(llvm::StringRef bytes);

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
};

/// The option whose value sets apart nearly every command `entries` compile
/// under, by its name: one naming a file of each unit that clice does not
/// know to leave out, which leaves the entries no command to share. Empty
/// when most entries share their commands, or their user content (-I, -D)
/// sets them apart.
std::string per_file_option(const CommandTable& commands, llvm::ArrayRef<CompilationEntry> entries);

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
    /// same ids. Its entries' commands live in `commands`.
    CompilationDatabase(FileTable& files, CommandTable& commands);

    CompilationDatabase(const CompilationDatabase&) = delete;
    CompilationDatabase& operator=(const CompilationDatabase&) = delete;

    FileTable& files() {
        return file_table;
    }

    CommandTable& commands() {
        return command_table;
    }

    /// Register a database file, or look up its id when already known;
    /// `path` may name a directory holding compile_commands.json. Nothing
    /// is read until load_source().
    SourceID add_source(const Spelling& path);

    /// The source registered under `path`, which may name a directory as
    /// add_source's does.
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
    /// count. On success the source's old entries are replaced; the
    /// commands stay in the table (path ids stay stable across reloads).
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
    /// successful load, then maintained by the project's database watch
    /// (Project::look_at_databases). A discovered source that vanished keeps
    /// serving its entries but yields to the present ones (see
    /// Project::source_order).
    bool present(SourceID id) const;
    void set_present(SourceID id, bool present);

    /// Register and load `path` in one step; the entry count on success.
    std::optional<std::size_t> load(llvm::StringRef path);

    /// Reload one source and report the per-file delta of the whole
    /// database against its state before the reload.
    ///
    /// Entry identity is the command hash (CommandTable::command_hash), the
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

    /// The entries writing `pch` (CommandTable::pch_output), in storage order.
    llvm::ArrayRef<CompilationEntry> pch_writers(Fid pch) const;

    /// Map each file's path_id to the command hashes of its entries in storage
    /// order (a file may own several entries with different flags; the
    /// first is its default selection) — the identity reload_and_diff()
    /// diffs on.
    llvm::DenseMap<Fid, llvm::SmallVector<std::string, 1>> command_hash_snapshot();

    /// Append one command no database file holds, a test's or one given on
    /// the command line, to an anonymous source and return its entry;
    /// nullopt when normalization fails.
    std::optional<CompilationEntry> add_command(llvm::StringRef directory,
                                                llvm::StringRef file,
                                                kotatsu::string_list_ref arguments);

    std::optional<CompilationEntry> add_command(llvm::StringRef directory,
                                                llvm::StringRef file,
                                                llvm::StringRef command);

private:
    /// Sort entry_list by (file, source, ordinal) and index its writers.
    void sort_entries();

    std::optional<CompilationEntry> append_test_command(Fid file,
                                                        std::optional<CommandID> normalized);

private:
    /// The process's file table (borrowed by the project that owns this
    /// database — nested databases share one id space).
    FileTable& file_table;

    CommandTable& command_table;

    /// Registered sources, by SourceID; their entries are entry_list's.
    struct Source {
        std::string path;
        bool loaded = false;
        bool present = false;
        std::vector<LoadInput> inputs;
    };

    std::vector<Source> source_files;

    /// find_source's answers by spelling, forgotten when a source is added:
    /// the question costs a stat and a realpath, and the rules' order of a
    /// file's entries asks it for every database they name.
    mutable llvm::StringMap<std::optional<SourceID>> found_sources;

    /// Every source's entries, sorted by (file, source, ordinal).
    std::vector<CompilationEntry> entry_list;

    /// The entries writing each precompiled header (see pch_writers),
    /// rebuilt with entry_list.
    llvm::DenseMap<Fid, llvm::SmallVector<CompilationEntry, 1>> writers;
};

}  // namespace clice
