/// The facts under a partition as modules: who names each entity, the edges
/// between modules with their cycles and layers, and the headers that stay
/// internal partitions.

module clice:analysis.module_graph;

import :analysis.facts;
import :analysis.partition;
import :support.prelude;

namespace clice::analysis {

/// Strongly connected components by Tarjan's algorithm: a component is
/// emitted after every component it reaches, so the result lists
/// dependencies first.
std::vector<llvm::SmallVector<std::uint32_t>>
    strongly_connected(llvm::ArrayRef<llvm::SmallVector<std::uint32_t>> successors);

/// The uses read backwards: who names each entity, what each file owns.
struct Reverse {
    /// Entity -> the scoped files naming it, a fragment's names charged to
    /// the files pasting it in; never the owner itself.
    std::vector<llvm::SmallVector<std::uint32_t, 4>> users;

    /// File -> the entities it owns, macros apart.
    std::vector<llvm::SmallVector<std::uint32_t>> owned;

    explicit Reverse(const Facts& facts);
};

/// The module-level view of the facts under one partition.
struct Graph {
    const Facts& facts;
    const Partition& partition;

    struct Edge {
        /// Entity -> the from-module's files naming it.
        std::map<std::uint32_t, llvm::SmallVector<std::uint32_t, 2>> entities;

        /// The entities a header of the from-module names.
        llvm::DenseSet<std::uint32_t> interface;
    };

    std::map<std::pair<std::uint32_t, std::uint32_t>, Edge> edges;

    /// Module -> the modules its headers name, which its interface would
    /// import.
    std::vector<llvm::SmallVector<std::uint32_t>> imports;

    /// Strongly connected components over `imports`, dependencies first.
    std::vector<llvm::SmallVector<std::uint32_t>> components;
    std::vector<std::uint32_t> component_of;
    std::vector<std::uint32_t> layer;

    Reverse reverse;

    /// (file, entity) pairs where the file declares the entity without
    /// defining it: inside one module the file needs only its own
    /// declaration, not the header providing the entity.
    llvm::DenseSet<std::pair<std::uint32_t, std::uint32_t>> forward_declared;

    /// File -> the scoped files (charged for fragments) naming what it
    /// owns, apart from those of its module that declare it themselves.
    std::vector<llvm::SmallVector<std::uint32_t>> dependents;

    /// Headers only their own module's sources use, directly or through
    /// other such headers: internal partitions, whose names never reach
    /// the module's interface. A header specializing an out-of-scope
    /// template has users no relation shows and never is one.
    std::vector<bool> internal;

    Graph(const Facts& facts, const Partition& partition);

    std::uint32_t weight(std::uint32_t from, std::uint32_t to) const;

    std::uint32_t cyclic_modules() const;

    /// The lightest set of edges whose removal leaves `members` acyclic,
    /// as (from, to) pairs. Exact over every bottom-up order for small
    /// components; larger ones place the module importing the least of
    /// the rest lowest, one at a time.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>>
        cut(llvm::ArrayRef<std::uint32_t> members) const;
};

}  // namespace clice::analysis
