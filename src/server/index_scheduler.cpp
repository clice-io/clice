#include "server/index_scheduler.h"

#include "compile/command.h"
#include "support/logging.h"

namespace clice {

namespace et = eventide;

IndexScheduler::IndexScheduler(WorkerPool& pool,
                               CompileGraph& compile_graph,
                               index::ProjectIndex& project_index,
                               ServerPathPool& path_pool,
                               et::event_loop& loop)
    : pool_(pool),
      compile_graph_(compile_graph),
      project_index_(project_index),
      path_pool_(path_pool),
      loop_(loop) {}

void IndexScheduler::enqueue(std::uint32_t path_id) {
    if(queued_.insert(path_id).second) {
        queue_.push_back(path_id);
    }
}

void IndexScheduler::on_user_activity() {
    user_active_ = true;
}

void IndexScheduler::build_initial_queue(CompilationDatabase& cdb,
                                         FuzzyGraph& graph) {
    auto files = cdb.files();

    struct FileEntry {
        std::uint32_t path_id;
        std::size_t in_deg;
    };

    llvm::SmallVector<FileEntry> entries;
    for(auto* file : files) {
        auto path_id = path_pool_.intern(file);
        entries.push_back({path_id, graph.in_degree(path_id)});
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const FileEntry& a, const FileEntry& b) {
                         return a.in_deg > b.in_deg;
                     });

    for(auto& entry : entries) {
        enqueue(entry.path_id);
    }

    LOG_INFO("IndexScheduler: {} files queued for indexing", queue_.size());
}

et::task<> IndexScheduler::run() {
    while(!queue_.empty()) {
        if(user_active_) {
            user_active_ = false;
            co_await et::sleep(idle_delay, loop_);
            continue;
        }

        auto path_id = queue_.front();
        queue_.erase(queue_.begin());
        queued_.erase(path_id);

        co_await index_one_file(path_id);
        indexed_count_++;
    }

    indexing_ = false;
    LOG_INFO("IndexScheduler: indexing complete ({} files)", indexed_count_);
    co_return;
}

et::task<> IndexScheduler::index_one_file(std::uint32_t path_id) {
    auto path = path_pool_.resolve(path_id);
    LOG_INFO("IndexScheduler: indexing {}", path.str());

    indexing_ = true;

    auto deps_ok = co_await compile_graph_.compile_deps(path_id, loop_);
    if(!deps_ok) {
        LOG_WARN("IndexScheduler: dependency compilation failed for {}", path.str());
        co_return;
    }

    worker::IndexParams params;
    params.file = path.str();

    auto pch = compile_graph_.get_pch(path_id);
    auto pcms = compile_graph_.get_pcms(path_id);

    std::vector<std::pair<std::string, std::string>> pcm_vec;
    for(auto& [name, pcm_path] : pcms) {
        pcm_vec.emplace_back(name.str(), pcm_path);
    }

    auto result = co_await pool_.send_stateless(params);
    if(!result.has_value()) {
        LOG_WARN("IndexScheduler: indexing request failed for {}", path.str());
        co_return;
    }

    if(!result->success) {
        LOG_WARN("IndexScheduler: indexing failed for {}: {}", path.str(), result->error);
    }

    co_return;
}

}  // namespace clice
