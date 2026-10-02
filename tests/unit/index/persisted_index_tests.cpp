#include <limits>

#include "test/test.h"
#include "index/manifest.h"
#include "index/project_index.h"
#include "index/serialization.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

ZEST_SUITE(PersistedIndex) {

llvm::StringRef bytes_of(const std::vector<std::uint8_t>& blob) {
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

ZEST_CASE(ManifestRoundTrip) {
    index::TUManifest manifest;
    manifest.global_gen = 7;
    manifest.built_at = 1234567;
    manifest.tu_fv = VersionID{300};
    // A root node, a multi-byte-varint line, and a parent that FOLLOWS its
    // child (the include tree resolves parent chains after appending).
    manifest.nodes = {
        {300, ~0u, 1    },
        {301, 2,   70000},
        {302, 0,   12   },
    };
    manifest.contributions = {
        {VersionID{300}, 0xdeadbeefdeadbeefull},
        {VersionID{302}, 42                   },
    };
    manifest.local_fanout = {
        {.symbol = 5, .files = {0, 1}},
        {.symbol = 9, .files = {1, 0}},
    };

    llvm::SmallString<256> buf;
    llvm::raw_svector_ostream os(buf);
    index::serialize_manifest(manifest, os);

    auto loaded = index::deserialize_manifest(buf.str());
    ASSERT(loaded);
    ASSERT(*loaded == manifest);
}

ZEST_CASE(ManifestJunkRejected) {
    ASSERT(!index::deserialize_manifest("not a flatbuffer").has_value());
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
    std::vector<std::uint32_t> absent;
    std::vector<std::uint64_t> local_symbols;
    std::vector<std::uint32_t> local_file_ends;
    std::vector<std::uint32_t> local_files;
};

ZEST_CASE(ManifestFanoutRejected) {
    ManifestBlobMirror valid;
    valid.format_version = index::index_format_version;
    valid.contribution_count = 1;
    valid.contributions = {1, 0, 0, 0, 0, 0, 0, 0, 0};
    valid.local_symbols = {5, 7};
    valid.local_file_ends = {1, 2};
    valid.local_files = {0, 0};

    auto decodes = [&](const ManifestBlobMirror& mirror) {
        auto blob = kota::codec::fbs::to_bytes(mirror);
        return blob.has_value() && index::deserialize_manifest(bytes_of(*blob)).has_value();
    };
    ASSERT(decodes(valid));

    // A fanout file must name one of the manifest's contributions.
    auto mirror = valid;
    mirror.local_files = {0, 1};
    ASSERT(!decodes(mirror));

    mirror = valid;
    mirror.local_symbols = {7, 5};
    ASSERT(!decodes(mirror));

    mirror = valid;
    mirror.local_file_ends = {1};
    ASSERT(!decodes(mirror));

    mirror = valid;
    mirror.local_file_ends = {1, 1};
    ASSERT(!decodes(mirror));
}

ZEST_CASE(ManifestCountMismatchRejected) {
    // A node count claiming more nodes than the payload holds must not
    // decode.
    ManifestBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.node_count = 2;
    mirror.nodes = {1, 0, 5};  // one node's worth of varints

    auto blob = kota::codec::fbs::to_bytes(mirror);
    ASSERT(blob);
    ASSERT(!index::deserialize_manifest(bytes_of(*blob)).has_value());
}

ZEST_CASE(ManifestVarintOverflowRejected) {
    // A ten-byte varint whose last byte carries more than value bit 63
    // would silently shift the excess out and decode to an unrelated small
    // id, redirecting contributions to another file.
    ManifestBlobMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.node_count = 1;
    mirror.nodes = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0x00, 0x00};

    auto blob = kota::codec::fbs::to_bytes(mirror);
    ASSERT(blob);
    ASSERT(!index::deserialize_manifest(bytes_of(*blob)).has_value());
}

/// A project whose FileVersion for `path` is referenced by one manifest of
/// `tu` (so garbage collection keeps it) and whose only symbol references
/// `path` through `pool`.
index::ProjectIndex build_project(clice::FileTable& pool,
                                  llvm::StringRef path,
                                  llvm::StringRef tu) {
    index::ProjectIndex project;
    auto path_id = pool.intern(Spelling::absolute(path));
    auto fv = pool.intern_version(path_id, 0xabcd);

    index::TUManifest manifest;
    manifest.tu_fv = pool.intern_version(pool.intern(Spelling::absolute(tu)), 0x1111);
    manifest.nodes = {
        {fv.raw, ~0u, 3}
    };
    manifest.contributions = {
        {fv, 777}
    };
    project.apply_manifest(pool, pool.intern(Spelling::absolute(tu)), std::move(manifest));

    auto& symbol = project.touch(42);
    symbol.name = "sym";
    symbol.reference_files.add(path_id.raw);
    return project;
}

/// The files a symbol's bitmap names, as the pool's ids.
std::vector<std::uint32_t> reference_files(const index::ProjectIndex& project,
                                           index::SymbolHash hash) {
    std::vector<std::uint32_t> files;
    project.each_reference_file(hash, [&](Fid file) { files.push_back(file.raw); });
    return files;
}

ZEST_CASE(GlobalRoundTripRemap) {
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    project.global_generation = 9;
    auto& manifest =
        project.manifests.find(pool.intern(Spelling::absolute("/proj/tu.cpp")))->second;
    manifest.global_gen = 9;

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);

    // The next session interns other paths first, so the same file gets a
    // different pool id; both the FileVersion table and the loaded bitmap
    // must follow the path, not the id.
    clice::FileTable fresh;
    fresh.intern(Spelling::absolute("/proj/opened-first.cpp"));
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ASSERT(loaded.load_global(buf.str(), fresh, pins));

    auto id = fresh.find(Spelling::absolute("/proj/used.h"));
    ASSERT(id);
    ASSERT(llvm::is_contained(reference_files(loaded, 42), id->raw));
    ASSERT(loaded.reference_count(42) == 1u);
    ASSERT(fresh.versions.size() == pool.versions.size());
    ASSERT(loaded.global_generation == 9u);

    // The blob pins the TU's manifest at the stamp it was saved under.
    auto tu_fv = fresh.version_ids.find(
        {*fresh.find(Spelling::absolute("/proj/tu.cpp")), std::uint64_t(0x1111)});
    ASSERT(tu_fv != fresh.version_ids.end());
    ASSERT(pins.size() == std::size_t(1));
    ASSERT(pins.find(tu_fv->second)->second == 9u);

    ASSERT(fresh.version_ids.contains({*id, std::uint64_t(0xabcd)}));
}

ZEST_CASE(GlobalRebaseKeepsChanges) {
    // The rows a write serialized read from the landed blob afterwards,
    // while rows changed across the write stay changed — and a write that
    // never landed gives its rows back.
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    auto other = pool.intern(Spelling::absolute("/proj/other.h"));

    std::string first;
    llvm::raw_string_ostream first_os(first);
    project.serialize_global(first_os, pool);
    project.touch(43).name = "late";
    project.touch(42).reference_files.add(other.raw);
    project.rebase(llvm::MemoryBuffer::getMemBufferCopy(first), pool);
    ASSERT(project.symbol_count() == 2u);
    ASSERT(project.identity_of(43)->name == "late");
    ASSERT(project.reference_count(42) == 2u);

    std::string second;
    llvm::raw_string_ostream second_os(second);
    project.serialize_global(second_os, pool);
    project.restore_unwritten();
    ASSERT(project.identity_of(43)->name == "late");
    ASSERT(project.reference_count(42) == 2u);

    // The base blob's path table is kept as it is, so a base row's bitmap
    // survives the next write byte for byte.
    std::string third;
    llvm::raw_string_ostream third_os(third);
    project.serialize_global(third_os, pool);
    clice::FileTable fresh;
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ASSERT(loaded.load_global(third, fresh, pins));
    ASSERT(loaded.symbol_count() == 2u);
    ASSERT(loaded.reference_count(42) == 2u);
    ASSERT(llvm::is_contained(reference_files(loaded, 42),
                              fresh.find(Spelling::absolute("/proj/other.h"))->raw));
}

ZEST_CASE(GlobalCollectsGarbage) {
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    // Interned but referenced by no manifest — must not reach disk. The
    // shared table keeps it: other consumers may still anchor on it.
    auto dead_id = pool.intern(Spelling::absolute("/proj/dead.h"));
    pool.intern_version(dead_id, 0xdead);

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);
    ASSERT(pool.version_ids.contains({dead_id, std::uint64_t(0xdead)}));

    clice::FileTable fresh;
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ASSERT(loaded.load_global(buf.str(), fresh, pins));
    ASSERT(!fresh.find(Spelling::absolute("/proj/dead.h")).has_value());
    ASSERT(fresh.find(Spelling::absolute("/proj/used.h")));
}

ZEST_CASE(GlobalVersionGate) {
    // Only the version slot is written: every other field reads back absent,
    // which is structurally valid — the verdict must hinge on the value.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    clice::FileTable pool;
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{});
    ASSERT(stale);
    ASSERT(!loaded.load_global(bytes_of(*stale), pool, pins).has_value());

    auto current = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ASSERT(current);
    ASSERT(loaded.load_global(bytes_of(*current), pool, pins));
    ASSERT(loaded.symbol_count() == 0u);
    ASSERT(pins.empty());

    ASSERT(!loaded.load_global("not a flatbuffer", pool, pins).has_value());
}

/// Field order MUST mirror GlobalBlob (project_index.cpp).
struct GlobalBlobMirror {
    std::uint32_t format_version = index::index_format_version;
    std::uint64_t generation = 0;
    std::uint32_t next_fv_id = 0;
    std::vector<std::uint32_t> fv_ids;
    std::vector<std::string> fv_paths;
    std::vector<std::uint64_t> fv_hashes;
    std::vector<std::string> paths;
    std::vector<std::uint64_t> sym_hashes;
    std::string sym_names;
    std::vector<std::uint32_t> sym_name_ends;
    std::string sym_args;
    std::vector<std::uint32_t> sym_args_ends;
    std::vector<std::uint64_t> sym_parents;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::uint16_t> sym_flags;
    std::vector<std::uint32_t> sym_files;
    std::vector<std::uint32_t> sym_reference_counts;
    std::vector<std::uint32_t> sym_bitmap_ends;
    std::vector<std::uint8_t> sym_bitmaps;
    std::vector<std::uint32_t> manifest_fvs;
    std::vector<std::uint64_t> manifest_gens;
    std::uint64_t search_generation = 0;
    std::vector<std::uint64_t> search_pending;

    /// Append a row; hashes must arrive ascending.
    void add_symbol(std::uint64_t hash,
                    llvm::StringRef name,
                    const std::vector<std::byte>& image,
                    std::uint64_t parent = 0,
                    std::uint32_t file = index::no_file) {
        sym_hashes.push_back(hash);
        sym_names += name;
        sym_name_ends.push_back(static_cast<std::uint32_t>(sym_names.size()));
        sym_args_ends.push_back(static_cast<std::uint32_t>(sym_args.size()));
        sym_parents.push_back(parent);
        sym_kinds.push_back(0);
        sym_flags.push_back(0);
        sym_files.push_back(file);
        sym_reference_counts.push_back(0);
        for(auto byte: image) {
            sym_bitmaps.push_back(static_cast<std::uint8_t>(byte));
        }
        sym_bitmap_ends.push_back(static_cast<std::uint32_t>(sym_bitmaps.size()));
    }
};

auto encode(const GlobalBlobMirror& mirror) {
    return kota::codec::fbs::to_bytes(mirror);
}

/// A path table covering ids 0..3, with the reference under test at 3.
std::vector<std::string> four_paths() {
    return {"/proj/0.h", "/proj/1.h", "/proj/2.h", "/proj/ref.h"};
}

ZEST_CASE(GlobalBitmapPayloadGate) {
    // A malformed reference bitmap must fail the whole load: normalized to
    // empty it would silently lose the symbol's reference files, with
    // nothing ever rebuilding them.
    clice::Bitmap bits;
    bits.add(3);
    GlobalBlobMirror mirror;
    mirror.paths = four_paths();
    mirror.add_symbol(42, "sym", index::write_bitmap(bits));

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto valid = encode(mirror);
    ASSERT(valid);
    index::ProjectIndex loaded;
    ASSERT(loaded.load_global(bytes_of(*valid), pool, pins));
    ASSERT(loaded.identity_of(42));
    ASSERT(llvm::is_contained(reference_files(loaded, 42),
                              pool.find(Spelling::absolute("/proj/ref.h"))->raw));

    // A malformed image after columns that decoded fine: the reject must
    // leave no partial state — file versions or symbols — that later
    // merges would build on and the next save persist.
    mirror.next_fv_id = 8;
    mirror.fv_ids = {7};
    mirror.fv_paths = {"/proj/partial.h"};
    mirror.fv_hashes = {0x1};
    mirror.add_symbol(43, "other", {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}});
    auto corrupt = encode(mirror);
    ASSERT(corrupt);
    index::ProjectIndex rejecting;
    clice::FileTable untouched;
    ASSERT(!rejecting.load_global(bytes_of(*corrupt), untouched, pins).has_value());
    ASSERT(rejecting.symbol_count() == 0u);
    ASSERT(untouched.versions.empty());
    ASSERT(!untouched.find(Spelling::absolute("/proj/partial.h")).has_value());
}

ZEST_CASE(UncoveredBitmapIdRejected) {
    // The writer emits a path-table entry for every id its bitmaps
    // reference; dropping an uncovered id would silently lose the symbol's
    // reference files while every manifest stays fresh.
    clice::Bitmap bits;
    bits.add(3);
    GlobalBlobMirror mirror;
    mirror.paths = {"/proj/0.h"};
    mirror.add_symbol(42, "sym", index::write_bitmap(bits));

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto uncovered = encode(mirror);
    ASSERT(uncovered);
    index::ProjectIndex loaded;
    ASSERT(!loaded.load_global(bytes_of(*uncovered), pool, pins).has_value());
    ASSERT(loaded.symbol_count() == 0u);

    mirror.paths = four_paths();
    auto covered = encode(mirror);
    ASSERT(covered);
    ASSERT(loaded.load_global(bytes_of(*covered), pool, pins));
    ASSERT(loaded.identity_of(42));
}

ZEST_CASE(GlobalDuplicateVersionsRejected) {
    // Version-table ids and (path, hash) pairs are both map keys in the
    // writer; a repeated id in particular would intern the earlier pair to
    // an id whose record names the later path, attributing contributions
    // to the wrong file.
    GlobalBlobMirror mirror;
    mirror.next_fv_id = 9;
    mirror.fv_ids = {7, 7};
    mirror.fv_paths = {"/proj/a.h", "/proj/b.h"};
    mirror.fv_hashes = {0x1, 0x2};

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto dup_id = encode(mirror);
    ASSERT(dup_id);
    index::ProjectIndex loaded;
    ASSERT(!loaded.load_global(bytes_of(*dup_id), pool, pins).has_value());
    ASSERT(pool.versions.empty());

    mirror.fv_ids = {7, 8};
    mirror.fv_paths = {"/proj/a.h", "/proj/a.h"};
    mirror.fv_hashes = {0x1, 0x1};
    auto dup_pair = encode(mirror);
    ASSERT(dup_pair);
    ASSERT(!loaded.load_global(bytes_of(*dup_pair), pool, pins).has_value());

    // The same path under two content hashes is the legitimate shape: two
    // observed versions of one file. Only the ids the blob carries map
    // into the table; the ones the writer garbage-collected map nowhere.
    mirror.fv_hashes = {0x1, 0x2};
    auto distinct = encode(mirror);
    ASSERT(distinct);
    ASSERT(loaded.load_global(bytes_of(*distinct), pool, pins));
    ASSERT(pool.versions.size() == std::size_t(2));
    ASSERT(loaded.runtime_version(7));
    ASSERT(loaded.runtime_version(8));
    ASSERT(!loaded.runtime_version(0).has_value());
}

ZEST_CASE(GlobalBadCounterRejected) {
    // Ids are handed out from the counter, so a legit writer's ids all sit
    // below it; a lagging counter would alias stored ids on the next
    // intern, and a sentinel one would insert a DenseMap reserved key.
    GlobalBlobMirror mirror;
    mirror.next_fv_id = 8;
    mirror.fv_ids = {7};
    mirror.fv_paths = {"/proj/a.h"};
    mirror.fv_hashes = {0x1};

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto ahead = encode(mirror);
    ASSERT(ahead);
    index::ProjectIndex loaded;
    ASSERT(loaded.load_global(bytes_of(*ahead), pool, pins));

    mirror.next_fv_id = 7;
    auto lagging = encode(mirror);
    ASSERT(lagging);
    ASSERT(!loaded.load_global(bytes_of(*lagging), pool, pins).has_value());

    mirror.next_fv_id = std::numeric_limits<std::uint32_t>::max() - 1;
    mirror.fv_ids = {};
    mirror.fv_paths = {};
    mirror.fv_hashes = {};
    auto reserved = encode(mirror);
    ASSERT(reserved);
    ASSERT(!loaded.load_global(bytes_of(*reserved), pool, pins).has_value());

    // A garbage high-water mark far beyond any real lineage rejects.
    mirror.next_fv_id = 0xf0000000;
    auto oversized = encode(mirror);
    ASSERT(oversized);
    ASSERT(!loaded.load_global(bytes_of(*oversized), pool, pins).has_value());
}

ZEST_CASE(GlobalRoundTripSymbolFacts) {
    clice::FileTable pool;
    index::ProjectIndex project;
    auto file = pool.intern(Spelling::absolute("/proj/facts.h"));
    auto& parent = project.touch(7);
    parent.name = "ns";
    parent.kind = SymbolKind::Namespace;
    auto& symbol = project.touch(42);
    symbol.name = "Box";
    symbol.args = "<int>";
    symbol.parent = 7;
    symbol.kind = SymbolKind::Struct;
    symbol.flags = index::SymbolFlags::HasDefinition | index::SymbolFlags::Specialization;
    symbol.file = file.raw;

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);

    // The file column follows the path across pools like the bitmaps do.
    clice::FileTable fresh;
    fresh.intern(Spelling::absolute("/proj/opened-first.cpp"));
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ASSERT(loaded.load_global(buf.str(), fresh, pins));
    auto restored = loaded.identity_of(42);
    ASSERT(restored);
    ASSERT(restored->name == "Box");
    ASSERT(restored->args == "<int>");
    ASSERT(restored->parent == 7u);
    ASSERT(static_cast<std::uint16_t>(restored->flags) == static_cast<std::uint16_t>(symbol.flags));
    auto moved = fresh.find(Spelling::absolute("/proj/facts.h"));
    ASSERT(moved);
    ASSERT(restored->file == moved->raw);
    ASSERT(loaded.identity_of(7)->file == index::no_file);
}

ZEST_CASE(UncoveredFileIdRejected) {
    // A file id the path table does not cover would resolve to whatever
    // this session interned at that id: reject it like an uncovered
    // bitmap id.
    GlobalBlobMirror mirror;
    mirror.add_symbol(42, "sym", index::write_bitmap(clice::Bitmap{}));

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto control = encode(mirror);
    ASSERT(control);
    index::ProjectIndex loaded;
    ASSERT(loaded.load_global(bytes_of(*control), pool, pins));

    mirror.sym_files = {5};
    auto uncovered = encode(mirror);
    ASSERT(uncovered);
    index::ProjectIndex rejecting;
    ASSERT(!rejecting.load_global(bytes_of(*uncovered), pool, pins).has_value());
    ASSERT(rejecting.symbol_count() == 0u);
}

ZEST_CASE(GlobalDuplicateSymbolRejected) {
    // Rows are looked up by binary search over the hash column, so it
    // must ascend strictly; a repeated hash would let one row shadow the
    // other's identity and reference bitmap while every manifest still
    // loads as fresh.
    clice::Bitmap bits;
    bits.add(3);
    GlobalBlobMirror mirror;
    mirror.paths = four_paths();
    mirror.add_symbol(42, "sym", index::write_bitmap(bits));
    mirror.add_symbol(42, "impostor", index::write_bitmap(bits));

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto dup = encode(mirror);
    ASSERT(dup);
    index::ProjectIndex loaded;
    ASSERT(!loaded.load_global(bytes_of(*dup), pool, pins).has_value());
    ASSERT(loaded.symbol_count() == 0u);

    mirror.sym_hashes = {42, 43};
    auto distinct = encode(mirror);
    ASSERT(distinct);
    ASSERT(loaded.load_global(bytes_of(*distinct), pool, pins));
    ASSERT(loaded.symbol_count() == std::size_t(2));

    mirror.sym_hashes = {43, 42};
    auto descending = encode(mirror);
    ASSERT(descending);
    ASSERT(!loaded.load_global(bytes_of(*descending), pool, pins).has_value());
}

ZEST_CASE(GlobalReservedKeysRejected) {
    // The two DenseMap sentinel key values can never sit in the in-memory
    // tables, so the writer can never emit them; a blob carrying one is
    // corrupt, and inserting it would corrupt (or assert in) the loader's
    // own containers.
    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    index::ProjectIndex loaded;

    {
        GlobalBlobMirror mirror;
        mirror.fv_ids = {0xffffffffu};
        mirror.fv_paths = {"/proj/a.h"};
        mirror.fv_hashes = {0x1};
        auto bytes = encode(mirror);
        ASSERT(bytes);
        ASSERT(!loaded.load_global(bytes_of(*bytes), pool, pins).has_value());
    }
    {
        clice::Bitmap bits;
        bits.add(3);
        GlobalBlobMirror mirror;
        mirror.paths = four_paths();
        mirror.add_symbol(~std::uint64_t(0), "sym", index::write_bitmap(bits));
        auto bytes = encode(mirror);
        ASSERT(bytes);
        ASSERT(!loaded.load_global(bytes_of(*bytes), pool, pins).has_value());
    }
    {
        GlobalBlobMirror mirror;
        mirror.add_symbol(42, "sym", index::write_bitmap(clice::Bitmap{}), ~std::uint64_t(0));
        auto bytes = encode(mirror);
        ASSERT(bytes);
        ASSERT(!loaded.load_global(bytes_of(*bytes), pool, pins).has_value());
    }
    {
        GlobalBlobMirror mirror;
        mirror.paths = {""};
        auto bytes = encode(mirror);
        ASSERT(bytes);
        ASSERT(!loaded.load_global(bytes_of(*bytes), pool, pins).has_value());
    }
    ASSERT(pool.versions.empty());
    ASSERT(loaded.symbol_count() == 0u);
}

ZEST_CASE(UnknownFileVersionsDetected) {
    GlobalBlobMirror mirror;
    mirror.next_fv_id = 4;
    mirror.fv_ids = {3};
    mirror.fv_paths = {"/proj/a.h"};
    mirror.fv_hashes = {0x1};
    auto bytes = encode(mirror);
    ASSERT(bytes);

    clice::FileTable pool;
    pool.intern_version(pool.intern(Spelling::absolute("/proj/opened-first.cpp")), 0x9);
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ASSERT(loaded.load_global(bytes_of(*bytes), pool, pins));
    auto known =
        pool.version_ids.find({*pool.find(Spelling::absolute("/proj/a.h")), std::uint64_t(0x1)})
            ->second;

    index::TUManifest manifest;
    manifest.tu_fv = VersionID{3};
    manifest.nodes = {
        {3, ~0u, 1}
    };
    manifest.contributions = {
        {VersionID{3}, 7}
    };
    auto imported = manifest;
    ASSERT(loaded.import_manifest(imported));
    ASSERT(imported.tu_fv == known);
    ASSERT(imported.nodes[0].file == known.raw);
    ASSERT(imported.contributions[0].first == known);

    manifest.nodes.push_back({2, ~0u, 2});
    ASSERT(!loaded.import_manifest(manifest));
    manifest.nodes.back().file = ~0u;
    ASSERT(!loaded.import_manifest(manifest));
}

ZEST_CASE(SharedTableLineages) {
    // Two projects' indexes over one file table: each keeps its own
    // persisted ids, so the second loads beside the first and writes its
    // manifests back under the ids it read them with.
    clice::FileTable first_pool;
    auto first = build_project(first_pool, "/lib/used.h", "/lib/tu.cpp");
    std::string first_bytes;
    llvm::raw_string_ostream first_os(first_bytes);
    first.serialize_global(first_os, first_pool);

    clice::FileTable second_pool;
    second_pool.intern_version(second_pool.intern(Spelling::absolute("/app/pad.cpp")), 0x5);
    auto second = build_project(second_pool, "/lib/used.h", "/app/tu.cpp");
    auto& written =
        second.manifests.find(second_pool.intern(Spelling::absolute("/app/tu.cpp")))->second;
    auto persisted = second.export_manifest(written);
    std::string second_bytes;
    llvm::raw_string_ostream second_os(second_bytes);
    second.serialize_global(second_os, second_pool);

    clice::FileTable shared;
    index::ProjectIndex first_loaded;
    index::ProjectIndex second_loaded;
    llvm::DenseMap<VersionID, std::uint64_t> first_pins;
    llvm::DenseMap<VersionID, std::uint64_t> second_pins;
    ASSERT(first_loaded.load_global(first_bytes, shared, first_pins));
    ASSERT(second_loaded.load_global(second_bytes, shared, second_pins));

    // The header both index is one version of the shared table.
    ASSERT(shared.versions.size() == std::size_t(3));
    auto header = shared.version_ids.find(
        {*shared.find(Spelling::absolute("/lib/used.h")), std::uint64_t(0xabcd)});
    ASSERT(header != shared.version_ids.end());

    auto imported = persisted;
    ASSERT(second_loaded.import_manifest(imported));
    ASSERT(shared.version(imported.tu_fv).fid == *shared.find(Spelling::absolute("/app/tu.cpp")));
    ASSERT(imported.contributions[0].first == header->second);
    ASSERT(second_pins.size() == std::size_t(1));
    ASSERT(second_pins.contains(imported.tu_fv));
    ASSERT(second_loaded.export_manifest(imported) == persisted);
}

};  // ZEST_SUITE(PersistedIndex)

}  // namespace
}  // namespace clice::testing
