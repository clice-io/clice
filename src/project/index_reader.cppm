module clice:project.index_reader;

import :config.config;
import :feature.query;
import :index.query;
import :project.project;
import :support.prelude;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;

namespace clice {

/// How much of a workspace a reader loads beside its index.
enum class IndexLoad : std::uint8_t {
    /// The index alone, its tables bound in place and fetched on first
    /// use; an index with no records yet is none to read.
    Tables,

    /// The index as the writer loads it, the artifact records with it:
    /// what walks the whole index.
    Saved,

    /// Saved, and the build from the databases the index remembers: its
    /// members, and the dependency graph the questions about files walk.
    Build,
};

struct ReadOptions {
    IndexLoad load = IndexLoad::Tables;

    /// Withhold from the reads a file whose content moved on since it was
    /// indexed, each file checked on disk once; off, they serve every file
    /// as it was indexed.
    bool check_disk = true;

    /// Whether there is nothing to read without an index; off, a workspace
    /// whose index is missing or unusable opens with an empty one — for a
    /// reader that only consults the index, as `clice inspect` does.
    bool required = true;
};

/// A workspace's persisted index, opened read-only for one command-line
/// run: the project over a file table of its own, which names files under
/// the workspace as the command line does, and the reads over its index.
class IndexReader {
public:
    /// Open the persisted index of `workspace` under `config`, loaded from
    /// it, in `configuration`, one `config` declares (see
    /// check_requested_configuration): the cache store and the blob
    /// database read-only, the index over the database's read snapshot
    /// (pinned for the reader's lifetime). Null, with the cause logged,
    /// when there is no usable index and one is required.
    static std::unique_ptr<IndexReader> open(const Spelling& workspace,
                                             Config config,
                                             llvm::StringRef configuration,
                                             ReadOptions options);

    /// What the answers about symbols and files read (feature::query).
    feature::query::Context context() const;

    /// The dialect a file's text is lexed in, as the server lexes it with
    /// no editor choices (clice::dialect).
    const clang::LangOptions& dialect(Fid file);

    /// What an answer lists as stale: `listed`, with the files the reads
    /// withheld, sorted, each once.
    std::vector<std::string> stale(std::vector<std::string> listed) const;

    FileTable files;

    Project project{files};

    index::FreshnessGate gate{files, {.check_disk = true}};

    index::IndexQuery query;

private:
    explicit IndexReader(ReadOptions options);
};

/// What a reader says of an index database the index reports damaged: a
/// writer discards one only when it meets the damage itself, which may sit
/// in records no `clice index` run reads, so the reader names the directory
/// to delete.
std::string damaged_index_error(const Project& project);

/// The file a command-line argument names: a relative one under the
/// workspace, whatever the process working directory.
Spelling argument_path(const Project& project, llvm::StringRef argument);

}  // namespace clice
