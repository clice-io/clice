#pragma once

#include <chrono>
#include <cstdint>

#include "server/compile_graph.h"
#include "server/fuzzy_graph.h"
#include "server/path_pool.h"
#include "server/worker_pool.h"

#include "index/project_index.h"

#include "eventide/async/async.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

class CompilationDatabase;

namespace et = eventide;

/// Schedules background indexing during idle periods.
/// Enqueues files for indexing, pauses when user is active,
/// and resumes when idle timer fires.
class IndexScheduler {
public:
    IndexScheduler(WorkerPool& pool,
                   CompileGraph& compile_graph,
                   index::ProjectIndex& project_index,
                   ServerPathPool& path_pool,
                   et::event_loop& loop);

    void enqueue(std::uint32_t path_id);
    void on_user_activity();

    void build_initial_queue(CompilationDatabase& cdb, FuzzyGraph& graph);

    et::task<> run();

    bool is_indexing() const { return indexing_; }
    std::size_t pending_count() const { return queue_.size(); }
    std::size_t indexed_count() const { return indexed_count_; }

private:
    et::task<> index_one_file(std::uint32_t path_id);

    WorkerPool& pool_;
    CompileGraph& compile_graph_;
    index::ProjectIndex& project_index_;
    ServerPathPool& path_pool_;
    et::event_loop& loop_;

    llvm::SmallVector<std::uint32_t> queue_;
    llvm::DenseSet<std::uint32_t> queued_;
    bool indexing_ = false;
    bool user_active_ = false;
    std::size_t indexed_count_ = 0;

    static constexpr auto idle_delay = std::chrono::milliseconds(3000);
};

}  // namespace clice
