#include "test/test.h"
#include "index/manifest.h"
#include "index/project_index.h"
#include "index/serialization.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

TEST_SUITE(PersistedIndex) {

llvm::StringRef bytes_of(const std::vector<std::uint8_t>& blob) {
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

TEST_CASE(ManifestRoundTrip) {
    index::TUManifest manifest;
    manifest.global_gen = 7;
    manifest.built_at = 1234567;
    manifest.tu_fv = 300;
    // A root node, a multi-byte-varint line, and a parent that FOLLOWS its
    // child (the include graph resolves parent chains after appending).
    manifest.nodes = {
        {300, ~0u, 1    },
        {301, 2,   70000},
        {302, 0,   12   },
    };
    manifest.contributions = {
        {300, 0xdeadbeefdeadbeefull},
        {302, 42                   },
    };

    llvm::SmallString<256> buf;
    llvm::raw_svector_ostream os(buf);
    index::serialize_manifest(manifest, os);

    auto loaded = index::deserialize_manifest(buf.str());
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(*loaded == manifest);
}

TEST_CASE(ManifestJunkRejected) {
    ASSERT_FALSE(index::deserialize_manifest("not a flatbuffer").has_value());
}

TEST_CASE(ManifestCountMismatchRejected) {
    // Field order MUST mirror ManifestBlob (manifest.cpp): a node count
    // claiming more nodes than the payload holds must not decode.
    struct ManifestBlobMirror {
        std::uint32_t format_version = 0;
        std::uint64_t global_gen = 0;
        std::uint64_t built_at = 0;
        std::uint32_t tu_fv = 0;
        std::uint32_t node_count = 0;
        std::uint32_t contribution_count = 0;
        std::vector<std::uint8_t> nodes;
        std::vector<std::uint8_t> contributions;
    };

    ManifestBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.node_count = 2;
    mirror.nodes = {1, 0, 5};  // one node's worth of varints

    auto blob = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(blob.has_value());
    ASSERT_FALSE(index::deserialize_manifest(bytes_of(*blob)).has_value());
}

/// A project whose FileVersion for `path` is referenced by one manifest of
/// `tu` (so garbage collection keeps it) and whose only symbol references
/// `path` through `pool`.
index::ProjectIndex build_project(clice::PathPool& pool, llvm::StringRef path, llvm::StringRef tu) {
    index::ProjectIndex project;
    auto path_id = pool.intern(path);
    auto fv = project.intern_file_version(path_id, 0xabcd);
    project.file_versions.find(fv)->second.size = 100;
    project.file_versions.find(fv)->second.mtime_ns = 5555;

    index::TUManifest manifest;
    manifest.tu_fv = project.intern_file_version(pool.intern(tu), 0x1111);
    manifest.nodes = {
        {fv, ~0u, 3}
    };
    manifest.contributions = {
        {fv, 777}
    };
    project.apply_manifest(pool.intern(tu), std::move(manifest));

    auto& symbol = project.symbols[42];
    symbol.name = "sym";
    symbol.reference_files.add(path_id);
    return project;
}

TEST_CASE(GlobalRoundTripRemap) {
    clice::PathPool pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    project.global_generation = 9;

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);

    // The next session interns other paths first, so the same file gets a
    // different pool id; both the FileVersion table and the loaded bitmap
    // must follow the path, not the id.
    clice::PathPool fresh;
    fresh.intern("/proj/opened-first.cpp");
    index::ProjectIndex loaded;
    ASSERT_TRUE(loaded.load_global(buf.str(), fresh));

    auto id = fresh.find("/proj/used.h");
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(loaded.symbols[42].reference_files.contains(*id));
    ASSERT_EQ(loaded.next_fv_id, project.next_fv_id);
    ASSERT_EQ(loaded.global_generation, 9u);

    auto fv_it = loaded.fv_ids.find({*id, std::uint64_t(0xabcd)});
    ASSERT_TRUE(fv_it != loaded.fv_ids.end());
    auto& record = loaded.file_versions.find(fv_it->second)->second;
    ASSERT_EQ(record.size, 100u);
    ASSERT_EQ(record.mtime_ns, 5555);
}

TEST_CASE(GlobalCollectsGarbage) {
    clice::PathPool pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    // Interned but referenced by no manifest — must not reach disk, and
    // must be dropped from memory by the write.
    auto dead_id = pool.intern("/proj/dead.h");
    project.intern_file_version(dead_id, 0xdead);

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);
    ASSERT_FALSE(project.fv_ids.contains({dead_id, std::uint64_t(0xdead)}));

    clice::PathPool fresh;
    index::ProjectIndex loaded;
    ASSERT_TRUE(loaded.load_global(buf.str(), fresh));
    ASSERT_FALSE(fresh.find("/proj/dead.h").has_value());
    ASSERT_TRUE(fresh.find("/proj/used.h").has_value());
}

TEST_CASE(GlobalVersionGate) {
    // Only the version slot is written: every other field reads back absent,
    // which is structurally valid — the verdict must hinge on the value.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    clice::PathPool pool;
    index::ProjectIndex loaded;

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{});
    ASSERT_TRUE(stale.has_value());
    ASSERT_FALSE(loaded.load_global(bytes_of(*stale), pool));

    auto current = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(loaded.load_global(bytes_of(*current), pool));
    ASSERT_TRUE(loaded.symbols.empty());

    ASSERT_FALSE(loaded.load_global("not a flatbuffer", pool));
}

TEST_CASE(UnknownFileVersionsDetected) {
    index::ProjectIndex project;
    auto known = project.intern_file_version(0, 0x1);

    index::TUManifest manifest;
    manifest.tu_fv = known;
    manifest.nodes = {
        {known, ~0u, 1}
    };
    ASSERT_TRUE(project.knows_file_versions(manifest));

    manifest.nodes.push_back({known + 1, ~0u, 2});
    ASSERT_FALSE(project.knows_file_versions(manifest));
}

};  // TEST_SUITE(PersistedIndex)

}  // namespace
}  // namespace clice::testing
