module;

#include "support/prelude.h"

module clice;

import :index.database;
import :index.indexer;
import :index.project_index;
import :index.shard;
import :index.tu_index;
import :support.bytes;
import :unittests.utils.index_internals;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :unittests.utils.tu_index_blob;
import :unittests.utils;
import :vfs.cache_store;

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectIndex, Tester) {

std::vector<std::byte> serialized;

/// Build the current unit's TU index and return the zero-copy reader the
/// merge path consumes; `serialized` keeps the bytes alive.
index::TUIndex build_view() {
    serialized = index::build_tu_index(*unit);
    return index::TUIndex::from_bytes(as_chars(serialized));
}

index::SymbolHash find_symbol(const index::ProjectIndex& project, llvm::StringRef name) {
    index::SymbolHash found = 0;
    project.for_each_symbol(
        [&](index::SymbolHash hash, const index::SymbolView& symbol, std::uint32_t) {
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
    project.for_each_reference_file(hash, [&](Fid file) { files.push_back(file.raw); });
    return files;
}

/// The TU-local id -> pool id mapping merge_symbols() consumes, as merge()
/// computes it.
llvm::SmallVector<Fid> intern_paths(const index::TUIndex& view, clice::FileTable& pool) {
    llvm::SmallVector<Fid> ids;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        ids.push_back(pool.intern(Spelling::absolute(view.path(i))));
    }
    return ids;
}

/// Commit what `index` holds into `db` as a save does, so another index
/// opening `db` reads it.
void commit(index::ProjectIndex& index, index::BlobDatabase& db) {
    index::SaveBatch batch;
    index::IndexReport report;
    index.begin_save(batch, report);
    ZASSERT(db.write(batch.blobs, batch.removals));
    ZASSERT(db.advance_read_snapshot());
}

ZEST_CASE(MergeCollectsExternalSymbols) {
    add_file("header.h", R"(
        int external_fn();
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        static int local_fn() { return 1; }
        int use() { return external_fn() + local_fn(); }
    )");
    kind = CompilationKind::Whole;
    ZASSERT(compile());

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto view = build_view();
    ZASSERT(view.loaded());
    project.merge_symbols(view, intern_paths(view, pool));

    auto external = find_symbol(project, "external_fn");
    ZASSERT(external != 0);
    // Referenced from both the header (declaration) and the focus file.
    ZASSERT(project.reference_count(external) >= 2);

    // Non-External symbols get no symbol record.
    ZASSERT(find_symbol(project, "local_fn") == 0u);
}

ZEST_CASE(MergeUnionsSymbolFacts) {
    add_file("shared.h", R"(
        namespace lib { int shared_fn(); }
    )");
    add_focus("user.cpp", R"(
        #include "shared.h"
        int use() { return lib::shared_fn(); }
    )");
    kind = CompilationKind::Whole;
    ZASSERT(compile());

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto view = build_view();
    ZASSERT(view.loaded());
    project.merge_symbols(view, intern_paths(view, pool));

    // A declaration-only unit places the symbol at its declaring header.
    auto hash = find_symbol(project, "shared_fn");
    ZASSERT(hash != 0);
    auto declared = project.find_symbol(hash);
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
    definer.add_focus("lib.cpp", R"(
        #include "shared.h"
        [[deprecated]] int lib::shared_fn() { return 1; }
    )");
    definer.kind = CompilationKind::Whole;
    ZASSERT(definer.compile());
    auto definer_bytes = index::build_tu_index(*definer.unit);
    auto definer_view = index::TUIndex::from_bytes(as_chars(definer_bytes));
    ZASSERT(definer_view.loaded());
    project.merge_symbols(definer_view, intern_paths(definer_view, pool));

    auto defined = project.find_symbol(hash);
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
    mover.add_focus("moved.cpp", R"(
        #include "shared.h"
        int lib::shared_fn() { return 2; }
    )");
    mover.kind = CompilationKind::Whole;
    ZASSERT(mover.compile());
    auto mover_bytes = index::build_tu_index(*mover.unit);
    auto mover_view = index::TUIndex::from_bytes(as_chars(mover_bytes));
    ZASSERT(mover_view.loaded());
    project.merge_symbols(mover_view, intern_paths(mover_view, pool));
    auto moved = pool.find(Spelling::absolute(mover_view.path(mover_view.path_count() - 1)));
    ZASSERT(moved);
    ZASSERT(project.find_symbol(hash)->file == moved->raw);
}

ZEST_CASE(MergePicksOneSpelling) {
    // Two units spelling one specialization differently must leave the
    // same name in the table whichever merges first.
    auto spelled_int = tu_index_blob();
    spelled_int.paths = {"/proj/main.cpp"};
    add_symbol(spelled_int, 42, {.name = "X", .args = "<int>"});
    auto spelled_signed = tu_index_blob();
    spelled_signed.paths = {"/proj/main.cpp"};
    add_symbol(spelled_signed, 42, {.name = "X", .args = "<signed int>"});
    auto int_bytes = tu_index_bytes(spelled_int);
    auto signed_bytes = tu_index_bytes(spelled_signed);

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&int_bytes,    &signed_bytes},
            std::pair{&signed_bytes, &int_bytes   }
    }) {
        index::ProjectIndex project{pool};
        auto first_view = index::TUIndex::from_bytes(*first);
        auto second_view = index::TUIndex::from_bytes(*second);
        project.merge_symbols(first_view, intern_paths(first_view, pool));
        project.merge_symbols(second_view, intern_paths(second_view, pool));
        ZASSERT(project.find_symbol(42)->args == "<int>");
    }
}

ZEST_CASE(LatestSignatureStands) {
    // Renaming a parameter keeps the function's hash: the unit merged last
    // tells how it reads now.
    auto before = tu_index_blob();
    before.paths = {"/proj/main.cpp"};
    add_symbol(before, 42, {.name = "scale", .signature = "(int x)"});
    auto after = tu_index_blob();
    after.paths = {"/proj/main.cpp"};
    add_symbol(after, 42, {.name = "scale", .signature = "(int count)"});
    auto before_bytes = tu_index_bytes(before);
    auto after_bytes = tu_index_bytes(after);

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto before_view = index::TUIndex::from_bytes(before_bytes);
    auto after_view = index::TUIndex::from_bytes(after_bytes);
    project.merge_symbols(before_view, intern_paths(before_view, pool));
    project.merge_symbols(after_view, intern_paths(after_view, pool));
    ZASSERT(project.find_symbol(42)->signature == "(int count)");
}

ZEST_CASE(DefinitionDecidesSignature) {
    // A header declaring `scale(int)` and the unit defining it with a named
    // parameter read the same function differently: the definition's
    // reading stands whatever merges first.
    auto declares = tu_index_blob();
    declares.paths = {"/proj/main.cpp"};
    add_symbol(declares, 42, {.name = "scale", .signature = "(int)"});
    auto defines = tu_index_blob();
    defines.paths = {"/proj/main.cpp"};
    add_symbol(defines,
               42,
               {
                   .name = "scale",
                   .signature = "(int count)",
                   .flags = index::SymbolFlags::HasDefinition,
               });
    auto declares_bytes = tu_index_bytes(declares);
    auto defines_bytes = tu_index_bytes(defines);

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&declares_bytes, &defines_bytes },
            std::pair{&defines_bytes,  &declares_bytes}
    }) {
        index::ProjectIndex project{pool};
        auto first_view = index::TUIndex::from_bytes(*first);
        auto second_view = index::TUIndex::from_bytes(*second);
        project.merge_symbols(first_view, intern_paths(first_view, pool));
        project.merge_symbols(second_view, intern_paths(second_view, pool));
        ZASSERT(project.find_symbol(42)->signature == "(int count)");
    }
}

ZEST_CASE(DefinitionDecidesTokenKind) {
    // A unit that sees only `struct S;` and one that defines `class S {}`
    // must leave the definition's kind whichever merges first.
    auto declares = tu_index_blob();
    declares.paths = {"/proj/main.cpp"};
    add_symbol(declares, 42, {.name = "S", .token_class = {.kind = SemanticTokenKind::Struct}});
    auto defines = tu_index_blob();
    defines.paths = {"/proj/main.cpp"};
    add_symbol(defines,
               42,
               {
                   .name = "S",
                   .flags = index::SymbolFlags::HasDefinition,
                   .token_class = {.kind = SemanticTokenKind::Class},
               });
    auto declares_bytes = tu_index_bytes(declares);
    auto defines_bytes = tu_index_bytes(defines);

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&declares_bytes, &defines_bytes },
            std::pair{&defines_bytes,  &declares_bytes}
    }) {
        index::ProjectIndex project{pool};
        auto first_view = index::TUIndex::from_bytes(*first);
        auto second_view = index::TUIndex::from_bytes(*second);
        project.merge_symbols(first_view, intern_paths(first_view, pool));
        project.merge_symbols(second_view, intern_paths(second_view, pool));
        ZASSERT(project.find_symbol(42)->token_class.kind == SemanticTokenKind::Class);
    }
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
    index::ProjectIndex project{pool};
    auto a = pool.intern(Spelling::absolute("/proj/a.h"));
    auto b = pool.intern(Spelling::absolute("/proj/b.h"));
    auto tu1 = pool.intern(Spelling::absolute("/proj/tu1.cpp"));
    auto tu2 = pool.intern(Spelling::absolute("/proj/tu2.cpp"));
    auto version_a = pool.intern_version(a, 0xa);
    auto version_b = pool.intern_version(b, 0xb);

    auto manifest_for =
        [&](VersionID tu_version,
            std::initializer_list<std::pair<VersionID, std::uint64_t>> contributions) {
            index::TUManifest manifest;
            manifest.built_at = 1;
            manifest.tu_version = tu_version;
            manifest.contributions = contributions;
            return manifest;
        };

    auto tu1_version = pool.intern_version(tu1, 0x1);
    auto tu2_version = pool.intern_version(tu2, 0x2);

    // TU 1 contributes h1 to file 1 and h2 to file 2.
    auto affected = project.apply_manifest(tu1,
                                           manifest_for(tu1_version,
                                                        {
                                                            {version_a, 100},
                                                            {version_b, 200}
    }));
    ZASSERT(affected.size() == std::size_t(2));
    ZASSERT(project.live_variants(a).size() == std::size_t(1));

    // TU 2 shares file 1's variant: the live set does not grow.
    project.apply_manifest(tu2,
                           manifest_for(tu2_version,
                                        {
                                            {version_a, 100}
    }));
    ZASSERT(project.live_variants(a).size() == std::size_t(1));

    // TU 1 re-indexes with a new variant for file 1 and drops file 2: both
    // hashes stay live on file 1 (TU 2 still holds the old one), file 2
    // loses its only contribution.
    project.apply_manifest(tu1,
                           manifest_for(tu1_version,
                                        {
                                            {version_a, 300}
    }));
    ZASSERT(project.live_variants(a).size() == std::size_t(2));
    ZASSERT(project.live_variants(b).empty());

    project.remove_manifest(tu2);
    auto live = project.live_variants(a);
    ZASSERT(live.size() == std::size_t(1));
    ZASSERT(live.front() == 300u);

    project.remove_manifest(tu1);
    ZASSERT(project.live_variants(a).empty());
    ZASSERT(project.live_variants(b).empty());
    ZASSERT(IndexInternals::derived_tables_empty(project));
}

ZEST_CASE(RepeatedAbsentPlace) {
    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto header = pool.intern(Spelling::absolute("/proj/a.h"));
    auto tu = pool.intern(Spelling::absolute("/proj/tu.cpp"));
    auto place = pool.intern_version(header, 0);
    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = pool.intern_version(tu, 0x1);
    manifest.absent = {place, place};

    project.apply_manifest(tu, std::move(manifest));
    auto held = project.manifest(tu);
    ZASSERT(held);
    ZASSERT(held->absent.size() == 1u);
    auto lookups = [&] {
        llvm::SmallVector<Fid> units;
        project.for_each_absent_lookup(header, [&](Fid unit) { units.push_back(unit); });
        return units;
    };
    ZASSERT(lookups() == llvm::SmallVector<Fid>{tu});
    project.remove_manifest(tu);
    ZASSERT(lookups().empty());
    ZASSERT(IndexInternals::derived_tables_empty(project));
}

ZEST_CASE(TwoImportedVersions) {
    // Modules built at different times read two versions of one header.
    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto header = pool.intern(Spelling::absolute("/proj/a.h"));
    auto tu = pool.intern(Spelling::absolute("/proj/tu.cpp"));
    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = pool.intern_version(tu, 0x1);
    manifest.imports = {pool.intern_version(header, 0x2), pool.intern_version(header, 0x3)};

    project.apply_manifest(tu, std::move(manifest));
    auto importers = [&] {
        llvm::SmallVector<Fid> units;
        project.for_each_importer(header, [&](Fid unit) { units.push_back(unit); });
        return units;
    };
    ZASSERT(importers() == llvm::SmallVector<Fid>{tu});
    project.remove_manifest(tu);
    ZASSERT(importers().empty());
    ZASSERT(IndexInternals::derived_tables_empty(project));
}

ZEST_CASE(MergedRecordsRoundTrip) {
    add_focus("main.cpp", R"(
        int global_value = 42;
        int reader() { return global_value; }
    )");
    kind = CompilationKind::Whole;
    ZASSERT(compile());

    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, {});
    ZASSERT(db != nullptr);

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto view = build_view();
    ZASSERT(view.loaded());
    auto file_ids_map = intern_paths(view, pool);
    project.merge_symbols(view, file_ids_map);
    auto focus = file_ids_map[view.path_count() - 1];
    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = pool.intern_version(focus, view.content_hash(view.path_count() - 1));
    project.apply_manifest(focus, std::move(manifest));
    commit(project, *db);

    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.open(*db) == index::LoadOutcome::Loaded);
    auto symbol = find_symbol(loaded, "global_value");
    ZASSERT(symbol != 0);
    auto fresh_id = fresh.find(Spelling::absolute(pool.resolve(focus)));
    ZASSERT(fresh_id);
    ZASSERT(ranges::contains(reference_files(loaded, symbol), fresh_id->raw));
    ZASSERT(loaded.indexed(*fresh_id));
}

ZEST_CASE(SymbolRecordsRoundTrip) {
    // Only the symbols having arguments or instantiation files list them;
    // a file set stores up to two ids raw and more as a roaring image.
    // Every one reads back as written, from the changes and from the
    // database.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, {});
    ZASSERT(db != nullptr);

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    std::vector<std::uint32_t> paths;
    for(int i = 0; i < 4; i += 1) {
        auto file = pool.intern(Spelling::absolute(std::format("/proj/{}.h", i)));
        paths.push_back(IndexInternals::path_id(project, file));
    }
    auto& scope = project.edit(10);
    scope.name = "ns";
    scope.kind = SymbolKind::Namespace;
    scope.reference_files.add(paths[0]);
    auto& member = project.edit(20);
    member.name = "f";
    member.args = "<int>";
    member.parent = 10;
    member.file = paths[1];
    member.reference_files.add(paths[1]);
    member.reference_files.add(paths[2]);
    auto& outsider = project.edit(30);
    outsider.name = "g";
    outsider.parent = 999;
    for(auto path: paths) {
        outsider.reference_files.add(path);
    }
    outsider.instantiation_files.add(paths[3]);

    auto check = [&](const index::ProjectIndex& index, clice::FileTable& table) {
        auto id_of = [&](llvm::StringRef path) {
            return table.intern(Spelling::absolute(path)).raw;
        };
        auto scope = index.find_symbol(10);
        ZASSERT(scope);
        ZEXPECT(scope->parent == 0u);
        ZEXPECT(scope->args.empty());
        ZEXPECT(scope->file == index::no_file);
        auto member = index.find_symbol(20);
        ZASSERT(member);
        ZEXPECT(member->parent == 10u);
        ZEXPECT(member->args == "<int>");
        ZEXPECT(member->file == id_of("/proj/1.h"));
        ZEXPECT(index.reference_count(20) == 2u);
        auto outsider = index.find_symbol(30);
        ZASSERT(outsider);
        ZEXPECT(outsider->parent == 999u);
        ZEXPECT(index.reference_count(10) == 1u);
        ZEXPECT(index.reference_count(30) == 4u);
        std::vector<std::uint32_t> referencing;
        index.for_each_reference_file(30, [&](Fid file) { referencing.push_back(file.raw); });
        ranges::sort(referencing);
        std::vector<std::uint32_t> expected{id_of("/proj/0.h"),
                                            id_of("/proj/1.h"),
                                            id_of("/proj/2.h"),
                                            id_of("/proj/3.h")};
        ranges::sort(expected);
        ZEXPECT(referencing == expected);
        std::vector<std::uint32_t> instantiating;
        index.for_each_instantiation_file(30, [&](Fid file) { instantiating.push_back(file.raw); });
        ZEXPECT(instantiating == std::vector{id_of("/proj/3.h")});
        std::size_t none = 0;
        index.for_each_instantiation_file(20, [&](Fid) { none += 1; });
        ZEXPECT(none == 0u);
    };
    check(project, pool);

    commit(project, *db);
    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.open(*db) == index::LoadOutcome::Loaded);
    check(loaded, fresh);
}

ZEST_CASE(LazyShardsStayPut) {
    // A reader fetches shards while a query still holds the ones it read
    // first: the fan-out of a position query resolves other files between
    // taking a shard and reading it again.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, {});
    ZASSERT(db != nullptr);

    clice::FileTable writer_files;
    index::ProjectIndex writer{writer_files};
    writer.edit(7).name = "x";
    commit(writer, *db);

    constexpr std::uint32_t count = 300;
    llvm::StringRef content = "int x = 1;\n";
    index::FileIndex file_index;
    file_index.relations[7].push_back({
        .kind = RelationKind::Definition,
        .range = {4, 5}
    });
    auto shard = index::write_shard(file_index, {}, content);
    clice::FileTable files;
    index::ProjectIndex project{files};
    std::vector<index::BlobDatabase::Blob> puts;
    for(std::uint32_t i = 0; i < count; i += 1) {
        auto file = files.intern(Spelling::absolute(std::format("/proj/f{}.cpp", i)));
        puts.push_back({index::BlobKind::Shard, IndexInternals::file_key(project, file), shard});
    }
    ZASSERT(db->write(puts, {}));
    ZASSERT(db->advance_read_snapshot());

    ZASSERT(project.open(*db) == index::LoadOutcome::Loaded);
    auto first = files.intern(Spelling::absolute("/proj/f0.cpp"));
    const auto* held = project.shard(first);
    ZASSERT(held != nullptr);
    for(std::uint32_t i = 1; i < count; i += 1) {
        ZASSERT(project.shard(files.intern(Spelling::absolute(std::format("/proj/f{}.cpp", i)))) !=
                nullptr);
    }
    ZASSERT(project.shard(first) == held);
    ZASSERT(held->content_hash() == llvm::xxh3_64bits(content));
    // A file the database holds no shard for is asked once and stays absent.
    ZASSERT(project.shard(files.intern(Spelling::absolute("/proj/none.cpp"))) == nullptr);
    ZASSERT(project.shard(files.intern(Spelling::absolute("/proj/none.cpp"))) == nullptr);
}

ZEST_CASE(SharedLocalAcrossUnits) {
    // One hash in two places: a `static` in util.h, which a.cpp includes
    // and b.cpp reads through its precompiled header, and a namespace of
    // the same name nested in an anonymous namespace of other.h, which
    // only c.cpp includes. A walk steps from a.cpp to b.cpp through the
    // header they share, and never into c.cpp's files — in the writer and
    // in a reader of the persisted records.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, {});
    ZASSERT(db != nullptr);

    constexpr index::SymbolHash helper = 99;
    clice::FileTable pool;
    index::ProjectIndex writer{pool};
    auto util = pool.intern_version(pool.intern(Spelling::absolute("/proj/util.h")), 0xaaaa);
    auto other = pool.intern_version(pool.intern(Spelling::absolute("/proj/other.h")), 0xbbbb);
    llvm::SmallVector<Fid> units;
    for(auto name: {"/proj/a.cpp", "/proj/b.cpp", "/proj/c.cpp"}) {
        auto tu = pool.intern(Spelling::absolute(name));
        auto header = llvm::StringRef(name).ends_with("c.cpp") ? other : util;
        index::TUManifest manifest;
        manifest.built_at = 1;
        manifest.tu_version = pool.intern_version(tu, 0x1111);
        manifest.nodes = {
            {manifest.tu_version.raw, ~0u, 1},
            {header.raw,              0,   1},
        };
        manifest.contributions = {
            {manifest.tu_version, 1}
        };
        if(llvm::StringRef(name).ends_with("b.cpp")) {
            manifest.imports = {header};
        } else {
            manifest.contributions.emplace_back(header, 2);
        }
        manifest.shared_locals = {
            {.symbol = helper, .files = {manifest.tu_version, header}}
        };
        writer.apply_manifest(tu, std::move(manifest));
        units.push_back(tu);
    }

    auto check = [&](const index::ProjectIndex& index, clice::FileTable& table) {
        auto ids = [&](std::initializer_list<llvm::StringRef> names) {
            llvm::SmallVector<Fid> fids;
            for(auto name: names) {
                fids.push_back(table.intern(Spelling::absolute(name)));
            }
            ranges::sort(fids);
            return fids;
        };
        for(auto group: {ids({"/proj/a.cpp", "/proj/b.cpp", "/proj/util.h"}),
                         ids({"/proj/c.cpp", "/proj/other.h"})}) {
            for(auto start: group) {
                llvm::SmallVector<Fid> reached;
                index.for_each_file_of_local(helper, start, [&](Fid file) {
                    reached.push_back(file);
                });
                ranges::sort(reached);
                ZEXPECT(reached == group);
            }
        }
    };
    check(writer, pool);

    commit(writer, *db);

    clice::FileTable files;
    index::ProjectIndex reader{files};
    ZASSERT(reader.open(*db) == index::LoadOutcome::Loaded);
    check(reader, files);

    // A unit leaving takes its set along: c.cpp's files no longer answer,
    // while the sets of a.cpp and b.cpp still join them through util.h.
    writer.remove_manifest(units[2]);
    llvm::SmallVector<Fid> reached;
    writer.for_each_file_of_local(helper,
                                  pool.intern(Spelling::absolute("/proj/c.cpp")),
                                  [&](Fid file) { reached.push_back(file); });
    ZEXPECT(reached.size() == 1u);
    llvm::SmallVector<Fid> joined;
    writer.for_each_file_of_local(helper,
                                  pool.intern(Spelling::absolute("/proj/a.cpp")),
                                  [&](Fid file) { joined.push_back(file); });
    ZEXPECT(joined.size() == 3u);
}

ZEST_CASE(SharedLocalSetsCounted) {
    // Two units list the same files for a symbol: the set answers until
    // the last of them leaves.
    constexpr index::SymbolHash helper = 99;
    clice::FileTable pool;
    index::ProjectIndex index{pool};
    auto x = pool.intern(Spelling::absolute("/proj/x.h"));
    auto x_version = pool.intern_version(x, 0xaaaa);
    auto y_version = pool.intern_version(pool.intern(Spelling::absolute("/proj/y.h")), 0xbbbb);
    llvm::SmallVector<Fid> units;
    for(auto name: {"/proj/a.cpp", "/proj/b.cpp"}) {
        auto tu = pool.intern(Spelling::absolute(name));
        index::TUManifest manifest;
        manifest.built_at = 1;
        manifest.tu_version = pool.intern_version(tu, 0x1111);
        manifest.nodes = {
            {manifest.tu_version.raw, ~0u, 1},
            {x_version.raw,           0,   1},
            {y_version.raw,           1,   1},
        };
        manifest.contributions = {
            {manifest.tu_version, 1},
            {x_version,           2},
            {y_version,           3},
        };
        manifest.shared_locals = {
            {.symbol = helper, .files = {x_version, y_version}}
        };
        index.apply_manifest(tu, std::move(manifest));
        units.push_back(tu);
    }
    auto reached = [&] {
        std::size_t count = 0;
        index.for_each_file_of_local(helper, x, [&](Fid) { count += 1; });
        return count;
    };
    ZEXPECT(reached() == 2u);
    index.remove_manifest(units[0]);
    ZEXPECT(reached() == 2u);
    index.remove_manifest(units[1]);
    ZEXPECT(reached() == 1u);
}

};  // ZEST_SUITE(ProjectIndex)

}  // namespace
}  // namespace clice::testing
