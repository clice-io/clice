#pragma once

#include "server/worker_pool.h"

#include "eventide/async/io/loop.h"
#include "eventide/async/runtime/sync.h"
#include "eventide/async/runtime/task.h"
#include "eventide/async/vocab/cancellation.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <memory>

namespace clice {

namespace et = eventide;

struct CompileUnit {
    std::uint32_t path_id = 0;
    llvm::SmallVector<std::uint32_t> dependencies;
    llvm::SmallVector<std::uint32_t> dependents;

    bool dirty = true;
    bool compiling = false;

    std::unique_ptr<et::cancellation_source> source =
        std::make_unique<et::cancellation_source>();

    std::unique_ptr<et::event> completion;
};

class CompileGraph {
public:
    explicit CompileGraph(WorkerPool& pool) : pool(pool) {}

    /// Register a compile unit with its dependencies.
    void register_unit(std::uint32_t path_id,
                       llvm::ArrayRef<std::uint32_t> deps);

    /// Cascade invalidation: mark dirty and cancel ongoing compilations.
    void update(std::uint32_t path_id);

    /// Ensure all dependencies of path_id are compiled.
    /// Returns true if all deps are ready, false otherwise.
    et::task<bool> compile_deps(std::uint32_t path_id, et::event_loop& loop);

    /// Cancel all ongoing compilations across all units.
    void cancel_all();

private:
    WorkerPool& pool;
    llvm::DenseMap<std::uint32_t, CompileUnit> units;

    et::task<> compile_impl(std::uint32_t path_id, et::event_loop& loop);
};

}  // namespace clice
