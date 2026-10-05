#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/database.h"
#include "index/project_index.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "vfs/cache_store.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectIndex, Tester) {

std::string wire;

/// Build the current unit's envelope and return the zero-copy reader the
/// merge path consumes; `wire` keeps the bytes alive.
index::TUIndex build_view() {
    wire = index::build_tu_index(*unit);
    return index::TUIndex::from_bytes(wire);
}

index::SymbolHash find_symbol(const index::ProjectIndex& project, llvm::StringRef name) {
    index::SymbolHash found = 0;
    project.for_each_symbol(
        [&](index::SymbolHash hash, const index::SymbolIdentity& symbol, std::uint32_t) {
            if(symbol.name == name) {
                found = hash;
            }
            return found == 0;
        });
    return found;
}

/// The files a symbol's bitmap names, as the pool's ids.
std::vector<std::uint32_t> reference_files(const index::ProjectIndex& project,
                                           index::SymbolHash hash) {
    std::vector<std::uint32_t> files;
    project.each_reference_file(hash, [&](Fid file) { files.push_back(file.raw); });
    return files;
}

/// The TU-local id -> pool id mapping merge() consumes, as Indexer::merge
/// computes it.
llvm::SmallVector<Fid> intern_paths(const index::TUIndex& view, clice::FileTable& pool) {
    llvm::SmallVector<Fid> ids;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        ids.push_back(pool.intern(Spelling::absolute(view.path(i))));
    }
    return ids;
}

llvm::StringRef bytes_of(const std::vector<std::uint8_t>& blob) {
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

ZEST_CASE(MergeCollectsExternalSymbols) {
    add_file("header.h", R"(
        int external_fn();
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        static int local_fn() { return 1; }
        int use() { return external_fn() + local_fn(); }
    )");
    ZASSERT(compile());

    clice::FileTable pool;
    index::ProjectIndex project;
    auto view = build_view();
    ZASSERT(view.loaded());
    ZASSERT(project.merge(view, intern_paths(view, pool)));

    auto external = find_symbol(project, "external_fn");
    ZASSERT(external != 0);
    // Referenced from both the header (declaration) and the main file.
    ZASSERT(project.reference_count(external) >= 2);

    // Non-External symbols never reach the project table.
    ZASSERT(find_symbol(project, "local_fn") == 0u);
}

ZEST_CASE(MergeUnionsSymbolFacts) {
    add_file("shared.h", R"(
        namespace lib { int shared_fn(); }
    )");
    add_main("user.cpp", R"(
        #include "shared.h"
        int use() { return lib::shared_fn(); }
    )");
    ZASSERT(compile());

    clice::FileTable pool;
    index::ProjectIndex project;
    auto view = build_view();
    ZASSERT(view.loaded());
    ZASSERT(project.merge(view, intern_paths(view, pool)));

    // A declaration-only unit places the symbol at its declaring header.
    auto hash = find_symbol(project, "shared_fn");
    ZASSERT(hash != 0);
    auto declared = project.identity_of(hash);
    ZASSERT(declared);
    ZASSERT(declared->parent == find_symbol(project, "lib"));
    ZASSERT(!index::has_flag(declared->flags, index::SymbolFlags::HasDefinition));
    auto header = pool.find(
        Spelling::absolute(view.path(0).ends_with("shared.h") ? view.path(0) : view.path(1)));
    ZASSERT(header);
    ZASSERT(declared->file == header->raw);

    // The defining unit moves the canonical file to its definition and
    // adds its bits to the union.
    Tester definer;
    definer.add_file("shared.h", R"(
        namespace lib { int shared_fn(); }
    )");
    definer.add_main("lib.cpp", R"(
        #include "shared.h"
        [[deprecated]] int lib::shared_fn() { return 1; }
    )");
    ZASSERT(definer.compile());
    std::string definer_wire = index::build_tu_index(*definer.unit);
    auto definer_view = index::TUIndex::from_bytes(definer_wire);
    ZASSERT(definer_view.loaded());
    ZASSERT(project.merge(definer_view, intern_paths(definer_view, pool)));

    auto defined = project.identity_of(hash);
    ZASSERT(defined);
    ZASSERT(index::has_flag(defined->flags, index::SymbolFlags::HasDefinition));
    ZASSERT(index::has_flag(defined->flags, index::SymbolFlags::Deprecated));
    auto definition =
        pool.find(Spelling::absolute(definer_view.path(definer_view.path_count() - 1)));
    ZASSERT(definition);
    ZASSERT(defined->file == definition->raw);
    ZASSERT(project.reference_count(hash) == 3u);

    // The table never retracts a unit's report, so a definition that
    // moved to another unit must still win the file over the old bit.
    Tester mover;
    mover.add_file("shared.h", R"(
        namespace lib { int shared_fn(); }
    )");
    mover.add_main("moved.cpp", R"(
        #include "shared.h"
        int lib::shared_fn() { return 2; }
    )");
    ZASSERT(mover.compile());
    std::string mover_wire = index::build_tu_index(*mover.unit);
    auto mover_view = index::TUIndex::from_bytes(mover_wire);
    ZASSERT(mover_view.loaded());
    ZASSERT(project.merge(mover_view, intern_paths(mover_view, pool)));
    auto moved = pool.find(Spelling::absolute(mover_view.path(mover_view.path_count() - 1)));
    ZASSERT(moved);
    ZASSERT(project.identity_of(hash)->file == moved->raw);
}

ZEST_CASE(MergePicksOneSpelling) {
    // Two units spelling one specialization differently must leave the
    // same name in the table whichever merges first.
    struct SymbolMirror {
        std::string name;
        std::string args;
        std::uint64_t parent = 0;
        std::uint8_t kind = 0;
        std::uint8_t scope = 0;
        std::uint16_t flags = 0;
        std::uint32_t file = index::no_file;
        std::vector<std::byte> reference_files;
    };

    struct EnvelopePrefixMirror {
        std::uint32_t format_version = index::index_format_version;
        std::int64_t built_at = 0;
        std::vector<std::string> paths = {"/proj/main.cpp"};
        std::vector<std::uint64_t> path_hashes;
        std::vector<index::IncludeNode> nodes;
        llvm::DenseMap<std::uint64_t, SymbolMirror> symbols{};
    };

    EnvelopePrefixMirror spelled_int;
    spelled_int.symbols[42] = {.name = "X", .args = "<int>"};
    EnvelopePrefixMirror spelled_signed;
    spelled_signed.symbols[42] = {.name = "X", .args = "<signed int>"};
    auto int_bytes = kota::codec::fbs::to_bytes(spelled_int);
    auto signed_bytes = kota::codec::fbs::to_bytes(spelled_signed);
    ZASSERT((int_bytes.has_value() && signed_bytes.has_value()));

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&*int_bytes,    &*signed_bytes},
            std::pair{&*signed_bytes, &*int_bytes   }
    }) {
        index::ProjectIndex project;
        auto first_view = index::TUIndex::from_bytes(bytes_of(*first));
        auto second_view = index::TUIndex::from_bytes(bytes_of(*second));
        ZASSERT(project.merge(first_view, intern_paths(first_view, pool)));
        ZASSERT(project.merge(second_view, intern_paths(second_view, pool)));
        ZASSERT(project.identity_of(42)->args == "<int>");
    }
}

ZEST_CASE(MergeRejectsBadBitmap) {
    // Field order MUST mirror the envelope layout (tu_index.cpp) up to
    // `symbols`: the builder always writes valid bitmap images, so a
    // malformed one has to be planted by hand.
    struct SymbolMirror {
        std::string name;
        std::string args;
        std::uint64_t parent = 0;
        std::uint8_t kind = 0;
        std::uint8_t scope = 0;
        std::uint16_t flags = 0;
        std::uint32_t file = index::no_file;
        std::vector<std::byte> reference_files;
    };

    struct EnvelopePrefixMirror {
        std::uint32_t format_version = 0;
        std::int64_t built_at = 0;
        std::vector<std::string> paths;
        std::vector<std::uint64_t> path_hashes;
        std::vector<index::IncludeNode> nodes;
        llvm::DenseMap<std::uint64_t, SymbolMirror> symbols{};
    };

    EnvelopePrefixMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.paths = {"/proj/main.cpp"};
    clice::Bitmap bits;
    bits.add(0);
    mirror.symbols[42] = {.name = "good_sym", .reference_files = index::write_bitmap(bits)};

    // Control: the mirror layout matches — the view sees the symbol and a
    // valid image merges.
    auto valid = kota::codec::fbs::to_bytes(mirror);
    ZASSERT(valid);
    auto valid_view = index::TUIndex::from_bytes(bytes_of(*valid));
    ZASSERT(valid_view.loaded());
    clice::FileTable pool;
    index::ProjectIndex accepting;
    ZASSERT(accepting.merge(valid_view, intern_paths(valid_view, pool)));
    ZASSERT(find_symbol(accepting, "good_sym") == 42u);

    // One malformed image rejects the whole result: merged bits would
    // persist behind versions that match the disk, with the lost ones
    // never rebuilt. The symbols that decoded fine must not stay behind.
    mirror.symbols[43] = {
        .name = "bad_sym",
        .reference_files = {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}},
    };
    auto corrupt = kota::codec::fbs::to_bytes(mirror);
    ZASSERT(corrupt);
    auto corrupt_view = index::TUIndex::from_bytes(bytes_of(*corrupt));
    ZASSERT(corrupt_view.loaded());
    index::ProjectIndex rejecting;
    ZASSERT(!rejecting.merge(corrupt_view, intern_paths(corrupt_view, pool)));
    ZASSERT(rejecting.symbol_count() == 0u);

    // An id past the path table is the same corruption in a decodable
    // coat: silently dropped, the symbol's relations would sit in a shard
    // its fan-out never visits. The reader hands reference-file ids out
    // raw, so this merge is the only gate.
    clice::Bitmap stray;
    stray.add(7);
    mirror.symbols[43] = {.name = "bad_sym", .reference_files = index::write_bitmap(stray)};
    auto out_of_range = kota::codec::fbs::to_bytes(mirror);
    ZASSERT(out_of_range);
    auto stray_view = index::TUIndex::from_bytes(bytes_of(*out_of_range));
    ZASSERT(stray_view.loaded());
    index::ProjectIndex bounding;
    ZASSERT(!bounding.merge(stray_view, intern_paths(stray_view, pool)));
    ZASSERT(bounding.symbol_count() == 0u);
}

ZEST_CASE(FileVersionInterning) {
    clice::FileTable pool;
    auto a = pool.intern_version(Fid{7}, 0x1111);
    ZASSERT(pool.intern_version(Fid{7}, 0x1111) == a);

    auto b = pool.intern_version(Fid{7}, 0x2222);
    ZASSERT(b != a);
    ZASSERT(pool.version(b).fid.raw == 7u);
    ZASSERT(pool.version(b).content_hash == 0x2222u);
}

ZEST_CASE(ManifestContributions) {
    clice::FileTable pool;
    index::ProjectIndex project;
    auto fv_a = pool.intern_version(Fid{1}, 0xa);
    auto fv_b = pool.intern_version(Fid{2}, 0xb);

    auto manifest_for = [&](VersionID tu_fv,
                            std::initializer_list<std::pair<VersionID, std::uint64_t>> rows) {
        index::TUManifest manifest;
        manifest.tu_fv = tu_fv;
        manifest.contributions = rows;
        return manifest;
    };

    auto tu1_fv = pool.intern_version(Fid{10}, 0x1);
    auto tu2_fv = pool.intern_version(Fid{11}, 0x2);

    // TU 1 contributes h1 to file 1 and h2 to file 2.
    auto affected = project.apply_manifest(pool,
                                           Fid{
                                               10
    },
                                           manifest_for(tu1_fv, {{fv_a, 100}, {fv_b, 200}}));
    ZASSERT(affected.size() == std::size_t(2));
    ZASSERT(project.live_variants(Fid{1}).size() == std::size_t(1));

    // TU 2 shares file 1's variant: the live set does not grow.
    project.apply_manifest(pool,
                           Fid{
                               11
    },
                           manifest_for(tu2_fv, {{fv_a, 100}}));
    ZASSERT(project.live_variants(Fid{1}).size() == std::size_t(1));

    // TU 1 re-indexes with a new variant for file 1 and drops file 2: both
    // hashes stay live on file 1 (TU 2 still holds the old one), file 2
    // loses its only contribution.
    project.apply_manifest(pool,
                           Fid{
                               10
    },
                           manifest_for(tu1_fv, {{fv_a, 300}}));
    ZASSERT(project.live_variants(Fid{1}).size() == std::size_t(2));
    ZASSERT(project.live_variants(Fid{2}).empty());

    project.remove_manifest(pool, Fid{11});
    auto live = project.live_variants(Fid{1});
    ZASSERT(live.size() == std::size_t(1));
    ZASSERT(live.front() == 300u);

    project.remove_manifest(pool, Fid{10});
    ZASSERT(project.contributions.empty());
}

ZEST_CASE(RepeatedAbsentPlace) {
    clice::FileTable pool;
    index::ProjectIndex project;
    auto place = pool.intern_version(Fid{1}, 0);
    index::TUManifest manifest;
    manifest.tu_fv = pool.intern_version(Fid{10}, 0x1);
    manifest.absent = {place, place};

    project.apply_manifest(pool, Fid{10}, std::move(manifest));
    ZASSERT(project.manifests[Fid{10}].absent.size() == std::size_t(1));
    project.remove_manifest(pool, Fid{10});
    ZASSERT(project.probed.empty());
}

ZEST_CASE(GlobalRoundTripWithRealMerge) {
    add_main("main.cpp", R"(
        int global_value = 42;
        int reader() { return global_value; }
    )");
    ZASSERT(compile());

    clice::FileTable pool;
    index::ProjectIndex project;
    auto view = build_view();
    ZASSERT(view.loaded());
    auto file_ids_map = intern_paths(view, pool);
    ZASSERT(project.merge(view, file_ids_map));

    // A manifest referencing the main file keeps its FileVersion alive
    // through the write's garbage collection.
    auto main_fv = pool.intern_version(file_ids_map[view.path_count() - 1],
                                       view.path_hash(view.path_count() - 1));
    index::TUManifest manifest;
    manifest.tu_fv = main_fv;
    project.apply_manifest(pool, file_ids_map[view.path_count() - 1], std::move(manifest));

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);

    clice::FileTable fresh;
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_global(buf.str(), fresh, pins));

    auto symbol = find_symbol(loaded, "global_value");
    ZASSERT(symbol != 0);
    auto main_path = pool.resolve(file_ids_map[view.path_count() - 1]);
    auto fresh_id = fresh.find(Spelling::absolute(main_path));
    ZASSERT(fresh_id);
    ZASSERT(llvm::is_contained(reference_files(loaded, symbol), fresh_id->raw));
}

ZEST_CASE(LazyShardsStayPut) {
    // A reader fetches shards while a query still holds the ones it read
    // first: the fan-out of a position query resolves other files between
    // taking a shard and reading it again.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, "");
    ZASSERT(db != nullptr);

    clice::FileTable writer_files;
    index::ProjectIndex writer;
    writer.touch(7).name = "x";
    std::vector<index::BlobDatabase::Blob> puts;
    std::string global;
    llvm::raw_string_ostream global_os(global);
    writer.serialize_global(global_os, writer_files);
    puts.push_back({index::IndexBlobKind::Global, "global", global});

    constexpr std::uint32_t count = 300;
    llvm::StringRef content = "int x = 1;\n";
    index::FileIndex rows;
    rows.relations[7].push_back({
        .kind = RelationKind::Definition,
        .range = {4, 5}
    });
    std::string shard;
    llvm::raw_string_ostream shard_os(shard);
    index::write_shard(rows, {}, content, shard_os);
    clice::FileTable files;
    index::ProjectIndex project;
    for(std::uint32_t i = 0; i < count; i += 1) {
        auto file = files.intern(Spelling::absolute(std::format("/proj/f{}.cpp", i)));
        puts.push_back({index::IndexBlobKind::Shard, project.key_of(files, file), shard});
    }
    ZASSERT(db->write(puts, {}).empty());
    ZASSERT(db->advance_read_snapshot());

    ZASSERT(project.open(*db, files));
    auto first = files.intern(Spelling::absolute("/proj/f0.cpp"));
    const auto* held = project.shard(first);
    ZASSERT(held != nullptr);
    for(std::uint32_t i = 1; i < count; i += 1) {
        ZASSERT(project.shard(files.intern(Spelling::absolute(std::format("/proj/f{}.cpp", i)))) !=
                nullptr);
    }
    ZASSERT(project.shard(first) == held);
    ZASSERT(held->content_hash() == llvm::xxh3_64bits(content));
    // A file the database holds no rows for is asked once and stays absent.
    ZASSERT(project.shard(files.intern(Spelling::absolute("/proj/none.cpp"))) == nullptr);
    ZASSERT(project.shard(files.intern(Spelling::absolute("/proj/none.cpp"))) == nullptr);
}

ZEST_CASE(ReaderFanoutAcrossUnits) {
    // A `static` in a shared header: each unit's manifest names the files
    // its own copy reaches, and a reader steps from one unit to the other
    // through the persisted reverse include graph.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, "");
    ZASSERT(db != nullptr);

    constexpr index::SymbolHash helper = 99;
    clice::FileTable pool;
    index::ProjectIndex writer;
    writer.global_generation = 3;
    auto header = pool.intern(Spelling::absolute("/proj/util.h"));
    auto header_fv = pool.intern_version(header, 0xaaaa);
    llvm::SmallVector<Fid> units;
    for(auto name: {"/proj/a.cpp", "/proj/b.cpp"}) {
        auto tu = pool.intern(Spelling::absolute(name));
        index::TUManifest manifest;
        manifest.global_gen = 3;
        manifest.tu_fv = pool.intern_version(tu, 0x1111);
        manifest.nodes = {
            {manifest.tu_fv.raw, ~0u, 1},
            {header_fv.raw,      0,   1},
        };
        manifest.contributions = {
            {manifest.tu_fv, 1},
            {header_fv,      2},
        };
        manifest.local_fanout = {
            {.symbol = helper, .files = {0, 1}}
        };
        writer.apply_manifest(pool, tu, std::move(manifest));
        units.push_back(tu);
    }

    std::vector<index::BlobDatabase::Blob> puts;
    for(auto tu: units) {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(writer.export_manifest(writer.manifests.find(tu)->second), os);
        puts.push_back({index::IndexBlobKind::Manifest, writer.key_of(pool, tu), bytes});
    }
    std::string global;
    llvm::raw_string_ostream global_os(global);
    writer.serialize_global(global_os, pool);
    puts.push_back({index::IndexBlobKind::Global, "global", global});
    ZASSERT(db->write(puts, {}).empty());
    ZASSERT(db->advance_read_snapshot());

    clice::FileTable files;
    index::ProjectIndex reader;
    ZASSERT(reader.open(*db, files));
    llvm::SmallVector<Fid> reached;
    reader.each_fanout_file(helper,
                            files.intern(Spelling::absolute("/proj/a.cpp")),
                            files,
                            [&](Fid file) { reached.push_back(file); });
    ZASSERT(reached.size() == 3u);
    for(auto name: {"/proj/a.cpp", "/proj/b.cpp", "/proj/util.h"}) {
        auto file = files.find(Spelling::absolute(name));
        ZASSERT(file);
        ZASSERT(llvm::is_contained(reached, *file));
    }
}

};  // ZEST_SUITE(ProjectIndex)

}  // namespace
}  // namespace clice::testing
