module clice:project.project_reader;

import :feature.query;
import :index.query;
import :project.project;
import :support.prelude;
import :vfs.file_table;
import :vfs.ids;

namespace clice {

/// A project opened for one command-line run that reads it: the project
/// over a file table of its own, which names files as the command line
/// does, and the reads over its index. The run opens it (Project::open).
struct ProjectReader {
    /// `check_disk`: withhold from the reads a file whose content moved on
    /// since it was indexed, each file checked on disk once; off, they
    /// serve every file as it was indexed.
    explicit ProjectReader(bool check_disk = true) :
        query(project.index, files, check_disk ? &gate : nullptr, nullptr) {}

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
};

/// What a reader says of an index database the index reports damaged: a
/// writer discards one only when it meets the damage itself, which may sit
/// in records no `clice index` run reads, so the reader names the directory
/// to delete.
std::string damaged_index_error(const Project& project);

}  // namespace clice
