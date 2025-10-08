
#include "Compiler/Compilation.h"
#include "Server/Indexer.h"
#include "Support/Logging.h"

namespace clice {

async::Task<> Indexer::index(llvm::StringRef path) {
    CompilationParams params;
    params.kind = CompilationUnit::Indexing;
    params.arguments = database.get_command(path).arguments;

    /// FIXME: We may want to stop the task in the future.
    /// params.stop;

    /// Check update?

    auto tu_index = co_await async::submit([&]() -> std::optional<index::TUIndex> {
        auto unit = compile(params);
        if(!unit) {
            logging::info("Fail to index for {}, because: {}", path, unit.error());
            return std::nullopt;
        }

        return index::TUIndex::build(*unit);
    });

    if(!tu_index) {
        co_return;
    }

    project_index.merge(*tu_index);
    in_memory_indices[project_index.path_pool.path_id(path)] = std::move(*tu_index);

    logging::info("Successfully index {}", path);
}

async::Task<> Indexer::schedule_next() {
    while(true) {
        while(waitings.empty()) {
            co_await update_event;
        }

        auto file_id = waitings.front();
        waitings.pop_front();

        auto file = project_index.path_pool.path(file_id);

        auto i = 0;
        for(; i < workings.size(); i++) {
            if(workings[i].empty()) {
                workings[i] = index(file);
                break;
            }
        }

        co_await workings[i];
        workings[i].release().destroy();
    }
}

async::Task<> Indexer::index_all() {
    for(auto& [file, cmd]: database) {
        waitings.push_back(project_index.path_pool.path_id(file));
    }

    auto max_count = std::max(std::thread::hardware_concurrency(), 4u);
    workings.resize(max_count);

    for(auto i = 0; i < max_count; i++) {
        auto task = schedule_next();
        task.schedule();
        task.dispose();
    }

    co_return;
}

}  // namespace clice
