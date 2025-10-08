
#include "Compiler/Compilation.h"
#include "Server/Indexer.h"
#include "Server/Convert.h"
#include "Support/Compare.h"
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

    /// FIXME: Currently, we merge index eagerly, I would like to improve
    /// this in the future.
    for(auto& [fid, index]: tu_index->file_indices) {
        auto path = tu_index->graph.path(tu_index->graph.path_id(fid));
        auto& merged_index = in_memory_indices[project_index.path_pool.path_id(path)];

        merged_index.merge(path, tu_index->graph.include_location_id(fid), index);
    }

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

    /// FIXME: Currently, we just reserve two thread for other kind of tasks,
    /// there may be a better way to handle this in the future ...
    workings.resize(max_count - 2);

    for(auto i = 0; i < max_count - 2; i++) {
        auto task = schedule_next();
        task.schedule();
        task.dispose();
    }

    co_return;
}

auto Indexer::lookup(llvm::StringRef path, std::uint32_t offset, RelationKind kind) -> Result {
    std::vector<proto::Location> locations;

    auto path_id = project_index.path_pool.path_id(path);
    auto index = in_memory_indices[path_id];
    auto occurrences = index.lookup(offset);
    if(occurrences.empty()) {
        co_return locations;
    }

    /// FIXME: We only handle first element now ...
    auto symbol_id = occurrences.front().target;
    auto refs = project_index.symbols[symbol_id].reference_files;

    /// FIXME: We may want to parallelize this ...
    for(auto file: refs) {
        auto& relations = in_memory_indices[file].relations[symbol_id];

        std::vector<LocalSourceRange> results;
        for(auto& [relation, _]: relations) {
            if(relation.kind & kind) {
                results.emplace_back(relation.range);
            }
        }

        llvm::StringRef path = project_index.path_pool.path(file);
        auto content = fs::read(path);
        if(!content) {
            continue;
        }

        /// FIXME: User server's encoding kind.
        ranges::sort(results, refl::less);
        PositionConverter converter(*content, PositionEncodingKind::UTF16);

        for(auto result: results) {
            auto begin = converter.toPosition(result.begin);
            auto end = converter.toPosition(result.end);
            locations.emplace_back(path.str(), proto::Range(begin, end));
        }
    }

    co_return locations;
}

auto Indexer::declaration(llvm::StringRef path, std::uint32_t offset) -> Result {
    co_return co_await lookup(path,
                              offset,
                              RelationKind(RelationKind::Declaration, RelationKind::Definition));
}

auto Indexer::definition(llvm::StringRef path, std::uint32_t offset) -> Result {
    co_return co_await lookup(path, offset, RelationKind::Definition);
}

auto Indexer::references(llvm::StringRef path, std::uint32_t offset) -> Result {
    co_return co_await lookup(
        path,
        offset,
        RelationKind(RelationKind::Declaration, RelationKind::Definition, RelationKind::Reference));
}

}  // namespace clice
