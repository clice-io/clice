module;

#include "support/prelude.h"

module clice:feature.counterparts;

import :feature.query;
import :syntax.dependency_graph;
import :vfs.file_table;
import :vfs.path;

namespace clice::feature {

/// A file that may be a counterpart of another, and what pairs the two: it
/// lies on the other side of the split between what declares (headers,
/// module interfaces) and what defines (sources, module implementation
/// units).
struct CounterpartCandidate {
    std::string path;

    /// The same file name but for the extension.
    bool same_name = false;

    /// The module whose interface one of them is and the other implements;
    /// empty when they pair otherwise.
    std::string module;

    /// How many of the declarations on the interface side the other side
    /// defines, by the index.
    std::uint32_t overlap = 0;

    /// Directory steps between the two files.
    std::uint32_t distance = 0;
};

/// The candidates best first, and whether the first is decisive: a client
/// goes to it without asking.
struct CounterpartRanking {
    std::vector<CounterpartCandidate> candidates;
    bool decisive = false;
};

/// Order counterpart candidates: shared declarations count above the same
/// name, which counts above a module pairing; then the most shared
/// declarations, the nearest directory, the path. The first is decisive
/// when it is the only one or beats every other: it has the other's shared
/// declarations and same name where the other has them, and at least as
/// many shared declarations; and it has one of those the other lacks, twice
/// its shared declarations, or — neither sharing any — a module pairing
/// the other lacks.
CounterpartRanking rank_counterparts(std::vector<CounterpartCandidate> candidates);

struct CounterpartEntry {
    std::string path;

    /// What pairs it with the file, as the user reads it ("same name",
    /// "defines 12 of 15 declarations").
    std::vector<std::string> reasons;
};

struct CounterpartsResult {
    std::string file;
    std::vector<CounterpartEntry> candidates;

    /// The candidate to go to without asking, when one is decisive.
    std::optional<std::string> preferred;
};

/// The files `path` pairs with — a header's sources, a source's headers, a
/// module's interface and implementation units — from what the build and
/// the index already know: the same name among the files of `graph`, the
/// build's dependency graph, declarations and their definitions across the
/// two, and the module declarations of `members`, the build's units.
query::Outcome<CounterpartsResult> counterparts(query::Context& ctx,
                                                const DependencyGraph& graph,
                                                llvm::ArrayRef<Fid> members,
                                                const Spelling& path);

}  // namespace clice::feature
