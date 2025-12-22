#pragma once

#include <deque>
#include <vector>

#include "Config.h"
#include "Convert.h"
#include "Async/Async.h"
#include "Compiler/Command.h"
#include "Index/MergedIndex.h"
#include "Index/ProjectIndex.h"
#include "Protocol/Protocol.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"

namespace clice {

class CompilationUnit;

class Indexer {
public:
    Indexer(CompilationDatabase& database,
            config::Config& config,
            const PositionEncodingKind& kind) :
        database(database), config(config), encoding_kind(kind) {}

    async::Task<> index(llvm::StringRef path);

    async::Task<> index(llvm::StringRef path, llvm::StringRef content);

    async::Task<> schedule_next();

    async::Task<> index_all();

    index::MergedIndex& get_index(std::uint32_t path_id) {
        auto [it, success] = in_memory_indices.try_emplace(path_id);
        if(!success) {
            return it->second;
        }

        auto it2 = project_index.indices.find(path_id);
        if(it2 != project_index.indices.end()) {
            auto path = project_index.path_pool.path(it2->second);
            it->second = index::MergedIndex::load(path);
        }

        return it->second;
    }

    using Result = async::Task<std::vector<proto::Location>>;

    void load_from_disk();

    void save_to_disk();

    auto lookup(llvm::StringRef path, std::uint32_t offset, RelationKind kind) -> Result;

    auto declaration(llvm::StringRef path, std::uint32_t offset) -> Result;

    auto definition(llvm::StringRef path, std::uint32_t offset) -> Result;

    auto references(llvm::StringRef path, std::uint32_t offset) -> Result;

    /// TODO: Calls ...

    /// TODO: Types ...

private:
    CompilationDatabase& database;

    config::Config& config;

    const PositionEncodingKind& encoding_kind;

    index::ProjectIndex project_index;

    PathMapping mapping;

    llvm::DenseMap<std::uint32_t, index::MergedIndex> in_memory_indices;

    /// Currently indexes tasks ...
    std::vector<async::Task<>> workings;

    /// FIXME: Use a LRU to make sure we won't index a file twice ...
    std::deque<std::uint32_t> waitings;

    async::Event update_event;
};

}  // namespace clice
