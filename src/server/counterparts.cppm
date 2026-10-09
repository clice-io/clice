module;

#include "modules/prelude.h"

module clice:server.counterparts;

import :server.query_commands;

namespace clice::query {

/// What pairs a file with one of its counterparts: a file on the other side
/// of the split between what declares (headers, module interfaces) and what
/// defines (sources, module implementation units).
struct Evidence {
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
struct Ranking {
    std::vector<Evidence> candidates;
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
Ranking rank_counterparts(std::vector<Evidence> candidates);

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
/// the index already know: the same name among the project's files,
/// declarations and their definitions across the two, and module
/// declarations. Needs the build.
Outcome<CounterpartsResult> counterparts(Context& ctx, const Spelling& path);

}  // namespace clice::query
