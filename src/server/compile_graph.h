#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

struct CompileUnit {
    std::uint32_t path_id = 0;

    /// Dependencies discovered lazily by resolve_fn.
    llvm::SmallVector<std::uint32_t> dependencies;

    /// Back-edges: units that depend on this unit.
    llvm::SmallVector<std::uint32_t> dependents;

    /// Whether resolve_fn has been called for this unit.
    bool resolved = false;

    bool dirty = true;
    bool compiling = false;

    /// Monotonic counter bumped by update(); used by compile_impl to detect
    /// stale completions without ABA risk from raw-pointer comparison.
    std::uint64_t generation = 0;

    std::unique_ptr<kota::cancellation_source> source =
        std::make_unique<kota::cancellation_source>();
    std::unique_ptr<kota::event> completion;
};

class CompileGraph {
public:
    /// Performs the actual compilation (e.g. produce PCM file).
    using dispatch_fn = std::function<kota::task<bool>(std::uint32_t path_id)>;

    /// Returns the dependency path_ids for a given path_id (called lazily on first compile).
    using resolve_fn = std::function<llvm::SmallVector<std::uint32_t>(std::uint32_t path_id)>;

    CompileGraph(dispatch_fn dispatch, resolve_fn resolve);

    /// Compile a unit and all its transitive dependencies.
    kota::task<bool> compile(std::uint32_t path_id);

    /// Compile all transitive module dependencies of path_id, but NOT path_id itself.
    /// Used for non-module files (plain .cpp) that import modules.
    kota::task<bool> compile_deps(std::uint32_t path_id);

    /// Mark path_id and all transitive dependents as dirty,
    /// cancelling any in-progress compilations.
    /// Returns the set of all path_ids that were marked dirty.
    llvm::SmallVector<std::uint32_t> update(std::uint32_t path_id);

    void cancel_all();

    bool has_unit(std::uint32_t path_id) const;
    bool is_dirty(std::uint32_t path_id) const;
    bool is_compiling(std::uint32_t path_id) const;

private:
    /// Get or create a unit, resolving its dependencies if needed.
    void ensure_resolved(std::uint32_t path_id);

    /// Internal compile with ancestor tracking for cycle detection.
    kota::task<bool> compile_impl(std::uint32_t path_id,
                                  llvm::DenseSet<std::uint32_t> ancestors,
                                  bool dispatch_self = true);

    /// Check if waiting on `target` would deadlock given our `ancestors` chain.
    /// Walks the dependency graph through compiling units to see if any dep
    /// transitively reaches a unit in our ancestor chain.
    bool has_wait_cycle(std::uint32_t target, const llvm::DenseSet<std::uint32_t>& ancestors) const;

    dispatch_fn dispatch;
    resolve_fn resolve;
    llvm::DenseMap<std::uint32_t, CompileUnit> units;
};

}  // namespace clice
