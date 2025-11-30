#include "Server/Indexer.h"

#include "Compiler/Compilation.h"
#include "Server/Convert.h"
#include "Support/Compare.h"
#include "Support/Logging.h"

namespace clice {

async::Task<> Indexer::index(llvm::StringRef path) {
    CompilationParams params;
    params.kind = CompilationUnit::Indexing;
    params.arguments_from_database = true;
    params.arguments = database.lookup(path).arguments;

    auto path_id = project_index.path_pool.path_id(path);
    auto& merged_index = get_index(path_id);
    if(!merged_index.need_update(project_index.path_pool.paths)) {
        LOG_INFO("Check update for {}, not need to update", path);
        co_return;
    }

    /// FIXME: We may want to stop the task in the future.
    /// params.stop;

    auto tu_index = co_await async::submit([&]() -> std::optional<index::TUIndex> {
        auto unit = compile(params);
        if(!unit) {
            LOG_INFO("Fail to index for {}, because: {}", path, unit.error());
            return std::nullopt;
        }

        return index::TUIndex::build(*unit);
    });

    if(!tu_index) {
        co_return;
    }

    auto path_map = project_index.merge(*tu_index);

    /// FIXME: Currently, we merge index eagerly, I would like to improve
    /// this in the future.
    for(auto& [fid, index]: tu_index->file_indices) {
        auto path_id = path_map[tu_index->graph.path_id(fid)];
        auto& merged_index = get_index(path_id);
        merged_index.merge(path_id, tu_index->graph.include_location_id(fid), index);
    }

    auto& index = get_index(path_id);
    for(auto& include: tu_index->graph.locations) {
        include.path_id = path_map[include.path_id];
    }
    index.merge(path_id,
                tu_index->built_at,
                std::move(tu_index->graph.locations),
                tu_index->main_file_index);

    LOG_INFO("Successfully index {}", path);
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
    for(auto& file: database.files()) {
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

void Indexer::load_from_disk() {
    std::string output_path = path::join(config.project.index_dir, "project.idx");
    if(auto content = fs::read(output_path); content && !content->empty()) {
        /// FIXME: from should return a expected ...
        project_index = index::ProjectIndex::from(content->data());
        LOG_INFO("Load project index form {} successfully", output_path);
    } else {
        LOG_INFO("Fail to load project index form {}", output_path);
    }

    /// FIXME: check indices update ....
}

void Indexer::save_to_disk() {
    if(auto err = fs::create_directories(config.project.index_dir)) {
        LOG_WARN("Fail to create index output dir: {}, because: {}", config.project.index_dir, err);
        return;
    }

    for(auto& [path_id, index]: in_memory_indices) {
        if(index.need_rewrite()) {
            auto path = project_index.path_pool.path(path_id);

            std::string output_path;
            if(auto it = project_index.indices.find(path_id); it != project_index.indices.end()) {
                output_path = project_index.path_pool.path(it->second);
            } else {
                output_path = path::join(
                    config.project.index_dir,
                    std::format("{}.{}.idx", path::filename(path), llvm::xxHash64(path)));
            }

            std::error_code err;
            llvm::raw_fd_ostream os(output_path, err, fs::CreationDisposition::CD_CreateAlways);
            if(err) {
                LOG_INFO("Fail to create output index file: {}, because: {}", output_path, err);
                continue;
            }

            index.serialize(os);

            auto opath_id = project_index.path_pool.path_id(output_path);
            project_index.indices.try_emplace(path_id, opath_id);
            LOG_INFO("Successfully save index for {} to {}", path, output_path);
        }
    }

    std::string output_path = path::join(config.project.index_dir, "project.idx");

    std::error_code err;
    llvm::raw_fd_ostream os(output_path, err, fs::CreationDisposition::CD_CreateAlways);
    if(err) {
        LOG_INFO("Fail to create output index file: {}, because: {}", output_path, err);
        return;
    }

    project_index.serialize(os);
    LOG_INFO("Successfully save project index to {}", output_path);
}

auto Indexer::lookup(llvm::StringRef path, std::uint32_t offset, RelationKind kind) -> Result {
    std::vector<proto::Location> locations;

    auto path_id = project_index.path_pool.path_id(path);
    auto& index = get_index(path_id);

    llvm::SmallVector<index::Occurrence> occurrences;
    index.lookup(offset, [&occurrences](const index::Occurrence& o) {
        occurrences.emplace_back(o);
        return true;
    });
    if(occurrences.empty()) {
        co_return locations;
    }

    /// FIXME: We only handle first element now ...
    auto symbol_id = occurrences.front().target;
    auto refs = project_index.symbols[symbol_id].reference_files;

    /// FIXME: We may want to parallelize this ...
    for(auto file: refs) {
        std::vector<LocalSourceRange> results;
        get_index(file).lookup(symbol_id, kind, [&results](const index::Relation& r) {
            results.emplace_back(r.range);
            return true;
        });

        llvm::StringRef path = project_index.path_pool.path(file);

        /// FIXME: Use the content stored in the merged index.
        auto content = fs::read(path);
        if(!content) {
            continue;
        }

        ranges::sort(results, refl::less);
        PositionConverter converter(*content, this->encoding_kind);

        for(auto result: results) {
            auto begin = converter.toPosition(result.begin);
            auto end = converter.toPosition(result.end);
            locations.emplace_back(mapping.to_uri(path), proto::Range(begin, end));
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
