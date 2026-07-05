#include "test/test.h"
#include "test/tester.h"
#include "index/project_index.h"
#include "index/tu_index.h"
#include "support/path_pool.h"

namespace clice::testing {
namespace {

TEST_SUITE(PathMapping, Tester) {

bool build_and_index(llvm::StringRef code, index::TUIndex& out) {
    add_main("main.cpp", code);
    if(!compile())
        return false;
    out = index::TUIndex::build(*unit);
    return true;
}

TEST_CASE(MergeLinksPaths) {
    index::TUIndex tu;
    ASSERT_TRUE(build_and_index("int x = 1;", tu));

    index::ProjectIndex project;
    auto file_ids_map = project.merge(tu);
    ASSERT_FALSE(project.path_pool.paths.empty());

    // The server already knows every merged path.
    PathPool server_pool;
    for(auto& path: project.path_pool.paths)
        server_pool.intern(path);
    project.link_server_paths(server_pool, file_ids_map);

    // Every project path is now mapped and round-trips both ways.
    for(std::uint32_t proj_id = 0; proj_id < project.path_pool.paths.size(); ++proj_id) {
        auto it = project.proj_to_server.find(proj_id);
        ASSERT_TRUE(it != project.proj_to_server.end());
        auto server_id = it->second;
        ASSERT_EQ(project.server_to_proj[server_id], proj_id);
        ASSERT_EQ(server_pool.resolve(server_id), project.path_pool.path(proj_id));
    }
}

TEST_CASE(IncrementalMiss) {
    index::TUIndex tu;
    ASSERT_TRUE(build_and_index("int y = 2;", tu));

    index::ProjectIndex project;
    auto file_ids_map = project.merge(tu);

    // Server knows nothing yet: linking is a no-op, mapping stays empty.
    PathPool server_pool;
    project.link_server_paths(server_pool, file_ids_map);
    ASSERT_TRUE(project.proj_to_server.empty());
    ASSERT_TRUE(project.server_to_proj.empty());

    // Once the server interns the path, a re-link picks it up.
    auto proj_id = file_ids_map.front();
    auto server_id = server_pool.intern(project.path_pool.path(proj_id));
    project.link_server_paths(server_pool, file_ids_map);

    auto it = project.proj_to_server.find(proj_id);
    ASSERT_TRUE(it != project.proj_to_server.end());
    ASSERT_EQ(it->second, server_id);
    ASSERT_EQ(project.server_to_proj[server_id], proj_id);
}

TEST_CASE(LoadResetsMapping) {
    index::TUIndex tu;
    ASSERT_TRUE(build_and_index("int z = 3;", tu));

    index::ProjectIndex project;
    auto file_ids_map = project.merge(tu);
    PathPool server_pool;
    for(auto& path: project.path_pool.paths)
        server_pool.intern(path);
    project.link_server_paths(server_pool, file_ids_map);
    ASSERT_FALSE(project.proj_to_server.empty());

    // Restoring from disk yields a fresh, empty mapping: server ids are
    // per-session and never serialized.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize(os);
    auto restored = index::ProjectIndex::from(buf.data());

    ASSERT_TRUE(restored.proj_to_server.empty());
    ASSERT_TRUE(restored.server_to_proj.empty());
}

};  // TEST_SUITE(PathMapping)
}  // namespace
}  // namespace clice::testing
