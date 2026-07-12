#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "compile/compilation.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/state/session_store.h"
#include "server/state/workspace.h"
#include "server/worker/worker_pool.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

TEST_SUITE(IndexerMerge) {

kota::event_loop loop;
Workspace workspace;
SessionStore store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
Indexer indexer{loop, workspace, pool, resolver, store};

struct IndexedTU {
    std::string data;     ///< Serialized TUIndex, as a worker would ship it.
    std::string tu_path;  ///< The TU's canonical path inside the index.
};

/// Index a real on-disk file in-process and serialize its TUIndex.
IndexedTU index_file(TempDir& tmp, llvm::StringRef file) {
    std::string resource = std::string(resource_dir());
    std::vector<std::string> args =
        {"clang++", "-fsyntax-only", "-resource-dir", resource, "-c", std::string(file)};

    CompilationParams cp;
    cp.kind = CompilationKind::Indexing;
    cp.directory = std::string(tmp.root);
    for(auto& arg: args) {
        cp.arguments.push_back(arg.c_str());
    }

    auto unit = compile(cp);
    if(!unit.completed()) {
        return {};
    }
    auto tu_index = index::TUIndex::build(unit);
    IndexedTU result;
    result.tu_path = tu_index.graph.paths.back();
    llvm::raw_string_ostream os(result.data);
    tu_index.serialize(os);
    return result;
}

TEST_CASE(MergeSkipsMovedDisk) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    indexer.merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    auto it = workspace.merged_indices.find(path_id);
    ASSERT_TRUE(it != workspace.merged_indices.end());
    ASSERT_EQ(it->second.content(), "int value() { return 1; }\n");

    // The disk moved on since the rows were indexed: merging them would
    // pair offsets with bytes they were not built from, so the merge must
    // be skipped and the last-known snapshot kept serving.
    tmp.touch("main.cpp", "int renamed() { return 2; }\n");
    indexer.merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(it->second.content(), "int value() { return 1; }\n");
    ASSERT_TRUE(it->second.has_contribution(indexed.tu_path));

    // Once the rows describe the settled content again, the merge lands.
    auto fresh = index_file(tmp, src);
    ASSERT_FALSE(fresh.data.empty());
    indexer.merge(fresh.data.data(), fresh.data.size());
    ASSERT_EQ(it->second.content(), "int renamed() { return 2; }\n");
}

};  // TEST_SUITE(IndexerMerge)

}  // namespace
}  // namespace clice::testing
