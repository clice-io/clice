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

/// Field order MUST mirror ManifestBlob (manifest.cpp).
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

TEST_CASE(ManifestCountMismatchRejected) {
    // A node count claiming more nodes than the payload holds must not
    // decode.
    ManifestBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.node_count = 2;
    mirror.nodes = {1, 0, 5};  // one node's worth of varints

    auto blob = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(blob.has_value());
    ASSERT_FALSE(index::deserialize_manifest(bytes_of(*blob)).has_value());
}

TEST_CASE(ManifestVarintOverflowRejected) {
    // A ten-byte varint whose last byte carries more than value bit 63
    // would silently shift the excess out and decode to an unrelated small
    // id, redirecting contributions to another file.
    ManifestBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.node_count = 1;
    mirror.nodes = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0x00, 0x00};

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
    auto& manifest = project.manifests.find(pool.intern("/proj/tu.cpp"))->second;
    manifest.global_gen = 9;

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);

    // The next session interns other paths first, so the same file gets a
    // different pool id; both the FileVersion table and the loaded bitmap
    // must follow the path, not the id.
    clice::PathPool fresh;
    fresh.intern("/proj/opened-first.cpp");
    index::ProjectIndex loaded;
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;
    ASSERT_TRUE(loaded.load_global(buf.str(), fresh, pins));

    auto id = fresh.find("/proj/used.h");
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(loaded.symbols[42].reference_files.contains(*id));
    ASSERT_EQ(loaded.next_fv_id, project.next_fv_id);
    ASSERT_EQ(loaded.global_generation, 9u);

    // The blob pins the TU's manifest at the stamp it was saved under.
    ASSERT_EQ(pins.size(), std::size_t(1));
    ASSERT_EQ(pins.find(manifest.tu_fv)->second, 9u);

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
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;
    ASSERT_TRUE(loaded.load_global(buf.str(), fresh, pins));
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
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{});
    ASSERT_TRUE(stale.has_value());
    ASSERT_FALSE(loaded.load_global(bytes_of(*stale), pool, pins));

    auto current = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(loaded.load_global(bytes_of(*current), pool, pins));
    ASSERT_TRUE(loaded.symbols.empty());
    ASSERT_TRUE(pins.empty());

    ASSERT_FALSE(loaded.load_global("not a flatbuffer", pool, pins));
}

/// Field order MUST mirror GlobalBlob (project_index.cpp).
struct GlobalBlobMirror {
    std::uint32_t format_version = 0;
    std::uint64_t generation = 0;
    std::uint32_t next_fv_id = 0;
    std::vector<std::uint32_t> fv_ids;
    std::vector<std::string> fv_paths;
    std::vector<std::uint64_t> fv_hashes;
    std::vector<std::uint64_t> fv_sizes;
    std::vector<std::int64_t> fv_mtimes;
    std::vector<std::uint64_t> sym_hashes;
    std::vector<std::string> sym_names;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::vector<std::byte>> sym_bitmaps;
    std::vector<std::uint32_t> manifest_fvs;
    std::vector<std::uint64_t> manifest_gens;
    std::vector<std::pair<std::uint32_t, std::string>> sym_paths;
};

TEST_CASE(GlobalBitmapPayloadGate) {
    // A malformed reference bitmap must fail the whole load: normalized to
    // empty it would silently lose the symbol's reference files, with
    // nothing ever rebuilding them.
    GlobalBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.sym_hashes = {42};
    mirror.sym_names = {"sym"};
    mirror.sym_kinds = {0};

    clice::Bitmap bits;
    bits.add(3);
    mirror.sym_bitmaps = {index::write_bitmap(bits)};
    mirror.sym_paths = {
        {3, "/proj/ref.h"}
    };

    clice::PathPool pool;
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;
    auto valid = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(valid.has_value());
    index::ProjectIndex loaded;
    ASSERT_TRUE(loaded.load_global(bytes_of(*valid), pool, pins));
    ASSERT_TRUE(loaded.symbols.contains(42));

    // A malformed image after columns that decoded fine: the reject must
    // leave no partial state — file versions or symbols — that later
    // merges would build on and the next save persist.
    mirror.fv_ids = {7};
    mirror.fv_paths = {"/proj/partial.h"};
    mirror.fv_hashes = {0x1};
    mirror.fv_sizes = {10};
    mirror.fv_mtimes = {10};
    mirror.sym_hashes = {42, 43};
    mirror.sym_names = {"sym", "other"};
    mirror.sym_kinds = {0, 0};
    mirror.sym_bitmaps = {
        index::write_bitmap(bits),
        {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}}
    };
    auto corrupt = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(corrupt.has_value());
    index::ProjectIndex rejecting;
    clice::PathPool untouched;
    ASSERT_FALSE(rejecting.load_global(bytes_of(*corrupt), untouched, pins));
    ASSERT_TRUE(rejecting.symbols.empty());
    ASSERT_TRUE(rejecting.file_versions.empty());
    ASSERT_FALSE(untouched.find("/proj/partial.h").has_value());
}

TEST_CASE(UncoveredBitmapIdRejected) {
    // The writer emits a path-table entry for every id its bitmaps
    // reference; dropping an uncovered id would silently lose the symbol's
    // reference files while every manifest stays fresh.
    GlobalBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.sym_hashes = {42};
    mirror.sym_names = {"sym"};
    mirror.sym_kinds = {0};
    clice::Bitmap bits;
    bits.add(3);
    mirror.sym_bitmaps = {index::write_bitmap(bits)};

    clice::PathPool pool;
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;
    auto uncovered = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(uncovered.has_value());
    index::ProjectIndex loaded;
    ASSERT_FALSE(loaded.load_global(bytes_of(*uncovered), pool, pins));
    ASSERT_TRUE(loaded.symbols.empty());

    mirror.sym_paths = {
        {3, "/proj/ref.h"}
    };
    auto covered = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(covered.has_value());
    ASSERT_TRUE(loaded.load_global(bytes_of(*covered), pool, pins));
    ASSERT_TRUE(loaded.symbols.contains(42));
}

TEST_CASE(GlobalDuplicateVersionsRejected) {
    // Version-table ids and (path, hash) pairs are both map keys in the
    // writer; a repeated id in particular would intern the earlier pair to
    // an id whose record names the later path, attributing contributions
    // to the wrong file.
    GlobalBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.fv_ids = {7, 7};
    mirror.fv_paths = {"/proj/a.h", "/proj/b.h"};
    mirror.fv_hashes = {0x1, 0x2};
    mirror.fv_sizes = {1, 2};
    mirror.fv_mtimes = {1, 2};

    clice::PathPool pool;
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;
    auto dup_id = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(dup_id.has_value());
    index::ProjectIndex loaded;
    ASSERT_FALSE(loaded.load_global(bytes_of(*dup_id), pool, pins));
    ASSERT_TRUE(loaded.file_versions.empty());

    mirror.fv_ids = {7, 8};
    mirror.fv_paths = {"/proj/a.h", "/proj/a.h"};
    mirror.fv_hashes = {0x1, 0x1};
    auto dup_pair = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(dup_pair.has_value());
    ASSERT_FALSE(loaded.load_global(bytes_of(*dup_pair), pool, pins));

    // The same path under two content hashes is the legitimate shape: two
    // observed versions of one file.
    mirror.fv_hashes = {0x1, 0x2};
    auto distinct = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(distinct.has_value());
    ASSERT_TRUE(loaded.load_global(bytes_of(*distinct), pool, pins));
    ASSERT_EQ(loaded.file_versions.size(), std::size_t(2));
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
