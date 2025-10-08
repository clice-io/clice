#pragma once

#include <deque>
#include <vector>

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
    Indexer(CompilationDatabase& database) : database(database) {}

    async::Task<> index(llvm::StringRef path);

    async::Task<> index(llvm::StringRef path, llvm::StringRef content);

    async::Task<> schedule_next();

    async::Task<> index_all();

    using Result = async::Task<std::vector<proto::Location>>;

    auto lookup(llvm::StringRef path, std::uint32_t offset, RelationKind kind) -> Result;

    auto declaration(llvm::StringRef path, std::uint32_t offset) -> Result;

    auto definition(llvm::StringRef path, std::uint32_t offset) -> Result;

    auto references(llvm::StringRef path, std::uint32_t offset) -> Result;

    /// TODO: Calls ...

    /// TODO: Types ...

private:
    CompilationDatabase& database;

    index::ProjectIndex project_index;

    llvm::DenseMap<std::uint32_t, index::MergedIndex> in_memory_indices;

    /// Currently indexes tasks ...
    std::vector<async::Task<>> workings;

    /// FIXME: Use a LRU to make sure we won't index a file twice ...
    std::deque<std::uint32_t> waitings;

    async::Event update_event;
};

}  // namespace clice
