#include "test/test.h"
#include "index/project_index.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

TEST_SUITE(PersistedIndex) {

/// A ProjectIndex whose only symbol references `path` through `pool`.
index::ProjectIndex reference_one(clice::PathPool& pool, llvm::StringRef path) {
    index::ProjectIndex project;
    auto& symbol = project.symbols[42];
    symbol.name = "sym";
    symbol.reference_files.add(pool.intern(path));
    return project;
}

TEST_CASE(SerializeCollectsGarbage) {
    clice::PathPool pool;
    auto project = reference_one(pool, "/proj/used.cpp");
    // Interned but referenced by nothing — must not reach disk.
    pool.intern("/proj/garbage.cpp");

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize(os, pool, {});

    clice::PathPool fresh;
    llvm::SmallVector<std::uint32_t> shards;
    auto loaded = index::ProjectIndex::from(buf.data(), buf.size(), fresh, shards);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(fresh.find("/proj/used.cpp").has_value());
    ASSERT_FALSE(fresh.find("/proj/garbage.cpp").has_value());
}

TEST_CASE(RemapAcrossSessions) {
    clice::PathPool pool;
    auto project = reference_one(pool, "/proj/used.cpp");

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize(os, pool, {});

    // The next session interns other paths first, so the same file gets a
    // different pool id; the loaded bitmap must follow the path, not the id.
    clice::PathPool fresh;
    fresh.intern("/proj/opened-first.cpp");
    llvm::SmallVector<std::uint32_t> shards;
    auto loaded = index::ProjectIndex::from(buf.data(), buf.size(), fresh, shards);
    ASSERT_TRUE(loaded.has_value());

    auto id = fresh.find("/proj/used.cpp");
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(loaded->symbols[42].reference_files.contains(*id));
}

TEST_CASE(ShardManifestRoundTrip) {
    clice::PathPool pool;
    auto project = reference_one(pool, "/proj/used.cpp");
    auto shard_owner = pool.intern("/proj/tu.cpp");

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize(os, pool, {shard_owner});

    clice::PathPool fresh;
    llvm::SmallVector<std::uint32_t> shards;
    auto loaded = index::ProjectIndex::from(buf.data(), buf.size(), fresh, shards);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(shards.size(), 1u);
    ASSERT_EQ(fresh.resolve(shards.front()), "/proj/tu.cpp");
}

TEST_CASE(OldBlobDiscarded) {
    clice::PathPool pool;
    llvm::SmallVector<std::uint32_t> shards;
    // Arbitrary bytes are rejected by verification, not misread.
    const char junk[] = "not a flatbuffer";
    ASSERT_FALSE(index::ProjectIndex::from(junk, sizeof(junk), pool, shards).has_value());
}

};  // TEST_SUITE(PersistedIndex)

}  // namespace
}  // namespace clice::testing
