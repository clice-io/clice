#pragma once

#include <deque>
#include <vector>

#include "Async/Async.h"
#include "Compiler/Command.h"
#include "Index/ProjectIndex.h"

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

private:
    CompilationDatabase& database;

    index::ProjectIndex project_index;

    llvm::DenseMap<std::uint32_t, index::TUIndex> in_memory_indices;

    /// Currently indexes tasks ...
    std::vector<async::Task<>> workings;

    /// FIXME: Use a LRU to make sure we won't index a file twice ...
    std::deque<std::uint32_t> waitings;

    async::Event update_event;
};

}  // namespace clice
