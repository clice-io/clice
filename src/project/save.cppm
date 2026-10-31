module clice:project.save;

import :index.database;
import :index.project_index;
import :support.prelude;

namespace clice {

/// What load_saved found.
struct SavedLoad {
    /// False when the database's header is of another format or corrupt:
    /// the writer discards the database and starts over, but a reader must
    /// report an unusable cache instead of an empty index.
    bool decoded = true;

    /// What the load changed: the units a rule keeps out or the build no
    /// longer compiles left the index, and the units whose command changed
    /// while no server ran are owed a reindex.
    index::IndexReport report;
};

/// Whether a database file's free pages are worth a compact copy: a quarter
/// of the file, and 64 MB at least.
bool shrink_due(const index::PageUsage& usage);

}  // namespace clice
