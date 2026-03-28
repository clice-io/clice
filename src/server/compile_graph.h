#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "eventide/async/async.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

namespace et = eventide;

struct CompileUnit {
    std::uint32_t path_id = 0;
    llvm::SmallVector<std::uint32_t> dependencies;
    llvm::SmallVector<std::uint32_t> dependents;

    bool dirty = true;
    bool compiling = false;

    /// Per-unit cancellation source. update() cancels the current source
    /// and creates a new one; waiters holding the old token observe cancellation.
    std::unique_ptr<et::cancellation_source> source = std::make_unique<et::cancellation_source>();

    /// Completion signal. When compiling == true, other callers
    /// co_await completion->wait() instead of starting a duplicate compilation.
    /// A new event is created each time compilation starts.
    std::unique_ptr<et::event> completion;
};

class CompileGraph {
public:
    /// Callback that performs the actual compilation of a single artifact (PCM/PCH).
    /// Returns true on success, false on failure.
    using dispatch_fn = std::function<et::task<bool>(std::uint32_t path_id)>;

    explicit CompileGraph(et::event_loop& loop, dispatch_fn dispatch);

    /// Register a compilation unit with its dependencies.
    /// Creates dependency nodes if they don't exist and populates
    /// reverse (dependent) edges.
    void register_unit(std::uint32_t path_id, llvm::ArrayRef<std::uint32_t> deps);

    /// Ensure all dependencies of path_id are compiled.
    /// Returns true if all dependencies are ready, false otherwise.
    /// Does NOT compile path_id itself (source files are compiled by Workers).
    et::task<bool> compile(std::uint32_t path_id);

    /// Mark path_id and all transitive dependents as dirty.
    /// Cancels any in-progress compilations via their cancellation sources.
    void update(std::uint32_t path_id);

    /// Cancel all in-progress compilations (for graceful shutdown).
    void cancel_all();

    // --- Accessors (for testing and diagnostics) ---

    bool has_unit(std::uint32_t path_id) const;
    bool is_dirty(std::uint32_t path_id) const;
    bool is_compiling(std::uint32_t path_id) const;

private:
    /// Compile a single dependency artifact. Handles deduplication
    /// (wait if already compiling), cycle detection (optional stack),
    /// and cancellation cleanup.
    et::task<bool> compile_impl(std::uint32_t path_id,
                                llvm::SmallVector<std::uint32_t>* stack = nullptr);

    et::event_loop& loop;
    dispatch_fn dispatch;
    llvm::DenseMap<std::uint32_t, CompileUnit> units;
};

}  // namespace clice
