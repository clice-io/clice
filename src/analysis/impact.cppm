/// What an edit to each file rebuilds: today, the units entering it; under
/// a partition, the importers of its module or the sources reaching its
/// internal partition.

module clice:analysis.impact;

import :analysis.annotation;
import :analysis.module_graph;
import :support.prelude;

namespace clice::analysis {

/// Rebuild weights of an edit to each file, by file id: the compile time of
/// the units it rebuilds, one per unit without that annotation.
struct Rebuild {
    std::vector<double> baseline;
    std::vector<double> partitioned;

    Rebuild(const Graph& graph, const Annotations& annotations);
};

}  // namespace clice::analysis
