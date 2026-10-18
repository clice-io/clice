module;

#include "modules/prelude.h"

module clice;

import :index.database;
import :index.project_index;
import :index.shard;
import :index.tu_index;
import :unittests.utils.temp_dir;
import :unittests.utils.tu_index_mirror;
import :unittests.utils;
import :unittests.utils.tester;
import :vfs.cache_store;

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectIndex, Tester) {

std::string serialized;

/// Build the current unit's TU index and return the zero-copy reader the
/// merge path consumes; `serialized` keeps the bytes alive.
index::TUIndex build_view() {
    serialized = index::build_tu_index(*unit);
    return index::TUIndex::from_bytes(serialized);
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
    index::ProjectIndex project;
    auto view = build_view();
    ZASSERT(view.loaded());
    project.merge(view, intern_paths(view, pool));

    auto external = find_symbol(project, "external_fn");
    ZASSERT(external != 0);
    // Referenced from both the header (declaration) and the focus file.
    ZASSERT(project.reference_count(external) >= 2);

    // Non-External symbols never reach the project table.
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
    index::ProjectIndex project;
    auto view = build_view();
    ZASSERT(view.loaded());
    project.merge(view, intern_paths(view, pool));

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
    definer.add_focus("lib.cpp", R"(
        #include "shared.h"
        [[deprecated]] int lib::shared_fn() { return 1; }
    )");
    definer.kind = CompilationKind::Whole;
    ZASSERT(definer.compile());
    std::string definer_bytes = index::build_tu_index(*definer.unit);
    auto definer_view = index::TUIndex::from_bytes(definer_bytes);
    ZASSERT(definer_view.loaded());
    project.merge(definer_view, intern_paths(definer_view, pool));

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
    mover.add_focus("moved.cpp", R"(
        #include "shared.h"
        int lib::shared_fn() { return 2; }
    )");
    mover.kind = CompilationKind::Whole;
    ZASSERT(mover.compile());
    std::string mover_bytes = index::build_tu_index(*mover.unit);
    auto mover_view = index::TUIndex::from_bytes(mover_bytes);
    ZASSERT(mover_view.loaded());
    project.merge(mover_view, intern_paths(mover_view, pool));
    auto moved = pool.find(Spelling::absolute(mover_view.path(mover_view.path_count() - 1)));
    ZASSERT(moved);
    ZASSERT(project.identity_of(hash)->file == moved->raw);
}

ZEST_CASE(MergePicksOneSpelling) {
    // Two units spelling one specialization differently must leave the
    // same name in the table whichever merges first.
    TUIndexMirror spelled_int;
    spelled_int.paths = {"/proj/main.cpp"};
    spelled_int.add_symbol(42, {.name = "X", .args = "<int>"});
    TUIndexMirror spelled_signed;
    spelled_signed.paths = {"/proj/main.cpp"};
    spelled_signed.add_symbol(42, {.name = "X", .args = "<signed int>"});
    auto int_bytes = spelled_int.bytes();
    auto signed_bytes = spelled_signed.bytes();

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&int_bytes,    &signed_bytes},
            std::pair{&signed_bytes, &int_bytes   }
    }) {
        index::ProjectIndex project;
        auto first_view = index::TUIndex::from_bytes(*first);
        auto second_view = index::TUIndex::from_bytes(*second);
        project.merge(first_view, intern_paths(first_view, pool));
        project.merge(second_view, intern_paths(second_view, pool));
        ZASSERT(project.identity_of(42)->args == "<int>");
    }
}

ZEST_CASE(LatestSignatureStands) {
    // Renaming a parameter keeps the function's hash: the unit merged last
    // tells how it reads now.
    TUIndexMirror before;
    before.paths = {"/proj/main.cpp"};
    before.add_symbol(42, {.name = "scale", .signature = "(int x)"});
    TUIndexMirror after;
    after.paths = {"/proj/main.cpp"};
    after.add_symbol(42, {.name = "scale", .signature = "(int count)"});
    auto before_bytes = before.bytes();
    auto after_bytes = after.bytes();

    clice::FileTable pool;
    index::ProjectIndex project;
    auto before_view = index::TUIndex::from_bytes(before_bytes);
    auto after_view = index::TUIndex::from_bytes(after_bytes);
    project.merge(before_view, intern_paths(before_view, pool));
    project.merge(after_view, intern_paths(after_view, pool));
    ZASSERT(project.identity_of(42)->signature == "(int count)");
}

ZEST_CASE(DefinitionDecidesSignature) {
    // A header declaring `scale(int)` and the unit defining it with a named
    // parameter read the same function differently: the definition's
    // reading stands whatever merges first.
    TUIndexMirror declares;
    declares.paths = {"/proj/main.cpp"};
    declares.add_symbol(42, {.name = "scale", .signature = "(int)"});
    TUIndexMirror defines;
    defines.paths = {"/proj/main.cpp"};
    defines.add_symbol(42,
                       {
                           .name = "scale",
                           .signature = "(int count)",
                           .flags = index::SymbolFlags::HasDefinition,
                       });
    auto declares_bytes = declares.bytes();
    auto defines_bytes = defines.bytes();

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&declares_bytes, &defines_bytes },
            std::pair{&defines_bytes,  &declares_bytes}
    }) {
        index::ProjectIndex project;
        auto first_view = index::TUIndex::from_bytes(*first);
        auto second_view = index::TUIndex::from_bytes(*second);
        project.merge(first_view, intern_paths(first_view, pool));
        project.merge(second_view, intern_paths(second_view, pool));
        ZASSERT(project.identity_of(42)->signature == "(int count)");
    }
}

ZEST_CASE(DefinitionDecidesTokenKind) {
    // A unit that sees only `struct S;` and one that defines `class S {}`
    // must leave the definition's kind whichever merges first.
    TUIndexMirror declares;
    declares.paths = {"/proj/main.cpp"};
    declares.add_symbol(42, {.name = "S", .token_class = {.kind = SemanticTokenKind::Struct}});
    TUIndexMirror defines;
    defines.paths = {"/proj/main.cpp"};
    defines.add_symbol(42,
                       {
                           .name = "S",
                           .flags = index::SymbolFlags::HasDefinition,
                           .token_class = {.kind = SemanticTokenKind::Class},
                       });
    auto declares_bytes = declares.bytes();
    auto defines_bytes = defines.bytes();

    clice::FileTable pool;
    for(auto [first, second]: {
            std::pair{&declares_bytes, &defines_bytes },
            std::pair{&defines_bytes,  &declares_bytes}
    }) {
        index::ProjectIndex project;
        auto first_view = index::TUIndex::from_bytes(*first);
        auto second_view = index::TUIndex::from_bytes(*second);
        project.merge(first_view, intern_paths(first_view, pool));
        project.merge(second_view, intern_paths(second_view, pool));
        ZASSERT(project.identity_of(42)->token_class.kind == SemanticTokenKind::Class);
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
    index::ProjectIndex project;
    auto a = pool.intern(Spelling::absolute("/proj/a.h"));
    auto b = pool.intern(Spelling::absolute("/proj/b.h"));
    auto tu1 = pool.intern(Spelling::absolute("/proj/tu1.cpp"));
    auto tu2 = pool.intern(Spelling::absolute("/proj/tu2.cpp"));
    auto fv_a = pool.intern_version(a, 0xa);
    auto fv_b = pool.intern_version(b, 0xb);

    auto manifest_for =
        [&](VersionID tu_fv,
            std::initializer_list<std::pair<VersionID, std::uint64_t>> contributions) {
            index::TUManifest manifest;
            manifest.tu_fv = tu_fv;
            manifest.contributions = contributions;
            return manifest;
        };

    auto tu1_fv = pool.intern_version(tu1, 0x1);
    auto tu2_fv = pool.intern_version(tu2, 0x2);

    // TU 1 contributes h1 to file 1 and h2 to file 2.
    auto affected = project.apply_manifest(pool,
                                           tu1,
                                           manifest_for(tu1_fv,
                                                        {
                                                            {fv_a, 100},
                                                            {fv_b, 200}
    }));
    ZASSERT(affected.size() == std::size_t(2));
    ZASSERT(project.live_variants(a).size() == std::size_t(1));

    // TU 2 shares file 1's variant: the live set does not grow.
    project.apply_manifest(pool,
                           tu2,
                           manifest_for(tu2_fv,
                                        {
                                            {fv_a, 100}
    }));
    ZASSERT(project.live_variants(a).size() == std::size_t(1));

    // TU 1 re-indexes with a new variant for file 1 and drops file 2: both
    // hashes stay live on file 1 (TU 2 still holds the old one), file 2
    // loses its only contribution.
    project.apply_manifest(pool,
                           tu1,
                           manifest_for(tu1_fv,
                                        {
                                            {fv_a, 300}
    }));
    ZASSERT(project.live_variants(a).size() == std::size_t(2));
    ZASSERT(project.live_variants(b).empty());

    project.remove_manifest(pool, tu2);
    auto live = project.live_variants(a);
    ZASSERT(live.size() == std::size_t(1));
    ZASSERT(live.front() == 300u);

    project.remove_manifest(pool, tu1);
    ZASSERT(project.contributions.empty());
}

ZEST_CASE(RepeatedAbsentPlace) {
    clice::FileTable pool;
    index::ProjectIndex project;
    auto header = pool.intern(Spelling::absolute("/proj/a.h"));
    auto tu = pool.intern(Spelling::absolute("/proj/tu.cpp"));
    auto place = pool.intern_version(header, 0);
    index::TUManifest manifest;
    manifest.tu_fv = pool.intern_version(tu, 0x1);
    manifest.absent = {place, place};

    project.apply_manifest(pool, tu, std::move(manifest));
    ZASSERT(project.manifests[tu].absent.size() == std::size_t(1));
    project.remove_manifest(pool, tu);
    ZASSERT(project.probed.empty());
}

ZEST_CASE(TwoImportedVersions) {
    // Modules built at different times read two versions of one header.
    clice::FileTable pool;
    index::ProjectIndex project;
    auto header = pool.intern(Spelling::absolute("/proj/a.h"));
    auto tu = pool.intern(Spelling::absolute("/proj/tu.cpp"));
    index::TUManifest manifest;
    manifest.tu_fv = pool.intern_version(tu, 0x1);
    manifest.imports = {pool.intern_version(header, 0x2), pool.intern_version(header, 0x3)};

    project.apply_manifest(pool, tu, std::move(manifest));
    ZASSERT(project.importers[header].contains(tu));
    project.remove_manifest(pool, tu);
    ZASSERT(project.importers.empty());
}

ZEST_CASE(GlobalRoundTripWithRealMerge) {
    add_focus("main.cpp", R"(
        int global_value = 42;
        int reader() { return global_value; }
    )");
    kind = CompilationKind::Whole;
    ZASSERT(compile());

    clice::FileTable pool;
    index::ProjectIndex project;
    auto view = build_view();
    ZASSERT(view.loaded());
    auto file_ids_map = intern_paths(view, pool);
    project.merge(view, file_ids_map);

    // A manifest referencing the focus file keeps its FileVersion alive
    // through the write's garbage collection.
    auto focus_fv = pool.intern_version(file_ids_map[view.path_count() - 1],
                                        view.path_hash(view.path_count() - 1));
    index::TUManifest manifest;
    manifest.tu_fv = focus_fv;
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
    auto focus_path = pool.resolve(file_ids_map[view.path_count() - 1]);
    auto fresh_id = fresh.find(Spelling::absolute(focus_path));
    ZASSERT(fresh_id);
    ZASSERT(llvm::is_contained(reference_files(loaded, symbol), fresh_id->raw));
}

ZEST_CASE(GlobalColumnsRoundTrip) {
    // Parents index the table, or a sparse column when outside it; only the
    // symbols having arguments or instantiation files list them; a file set
    // stores up to two ids raw and more as a roaring image. Every one reads
    // back as written, and again after a rewrite copies the entries over.
    clice::FileTable pool;
    index::ProjectIndex project;
    std::vector<Fid> files;
    for(int i = 0; i < 4; i += 1) {
        files.push_back(pool.intern(Spelling::absolute(std::format("/proj/{}.h", i))));
    }
    auto& scope = project.touch(10);
    scope.name = "ns";
    scope.kind = SymbolKind::Namespace;
    scope.reference_files.add(files[0].raw);
    auto& member = project.touch(20);
    member.name = "f";
    member.args = "<int>";
    member.parent = 10;
    member.file = files[1].raw;
    member.reference_files.add(files[1].raw);
    member.reference_files.add(files[2].raw);
    auto& outsider = project.touch(30);
    outsider.name = "g";
    outsider.parent = 999;
    for(auto file: files) {
        outsider.reference_files.add(file.raw);
    }
    outsider.instantiation_files.add(files[3].raw);

    auto check = [&](const index::ProjectIndex& index, const clice::FileTable& table) {
        auto id_of = [&](llvm::StringRef path) {
            return table.find(Spelling::absolute(path))->raw;
        };
        auto scope = index.identity_of(10);
        ZASSERT(scope);
        ZEXPECT(scope->parent == 0u);
        ZEXPECT(scope->args.empty());
        ZEXPECT(scope->file == index::no_file);
        auto member = index.identity_of(20);
        ZASSERT(member);
        ZEXPECT(member->parent == 10u);
        ZEXPECT(member->args == "<int>");
        ZEXPECT(member->file == id_of("/proj/1.h"));
        ZEXPECT(index.reference_count(20) == 2u);
        auto outsider = index.identity_of(30);
        ZASSERT(outsider);
        ZEXPECT(outsider->parent == 999u);
        ZEXPECT(index.reference_count(10) == 1u);
        ZEXPECT(index.reference_count(30) == 4u);
        std::vector<std::uint32_t> referencing;
        index.each_reference_file(30, [&](Fid file) { referencing.push_back(file.raw); });
        std::ranges::sort(referencing);
        std::vector<std::uint32_t> expected{id_of("/proj/0.h"),
                                            id_of("/proj/1.h"),
                                            id_of("/proj/2.h"),
                                            id_of("/proj/3.h")};
        std::ranges::sort(expected);
        ZEXPECT(referencing == expected);
        std::vector<std::uint32_t> instantiating;
        index.each_instantiation_file(30, [&](Fid file) { instantiating.push_back(file.raw); });
        ZEXPECT(instantiating == std::vector{id_of("/proj/3.h")});
        std::size_t none = 0;
        index.each_instantiation_file(20, [&](Fid) { none += 1; });
        ZEXPECT(none == 0u);
    };

    llvm::SmallString<4096> first;
    llvm::raw_svector_ostream first_os(first);
    project.serialize_global(first_os, pool);
    clice::FileTable fresh;
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_global(first.str(), fresh, pins));
    check(loaded, fresh);

    llvm::SmallString<4096> second;
    llvm::raw_svector_ostream second_os(second);
    loaded.serialize_global(second_os, fresh);
    clice::FileTable again;
    index::ProjectIndex reloaded;
    ZASSERT(reloaded.load_global(second.str(), again, pins));
    check(reloaded, again);
}

ZEST_CASE(WidePathIds) {
    // File ids take the width of the path table, which the files only the
    // reverse include graph names grow after the symbols are written.
    clice::FileTable pool;
    index::ProjectIndex project;
    auto file_of = [&](int i) {
        return pool.intern(Spelling::absolute(std::format("/proj/{}.h", i)));
    };
    auto& symbol = project.touch(10);
    symbol.name = "x";
    symbol.file = file_of(0).raw;
    symbol.reference_files.add(file_of(0).raw);
    auto unit = file_of(1000);
    for(int i = 1; i <= 300; i += 1) {
        project.contributions[file_of(i)][unit] = 1;
    }

    llvm::SmallString<4096> bytes;
    llvm::raw_svector_ostream os(bytes);
    project.serialize_global(os, pool);
    clice::FileTable fresh;
    index::ProjectIndex loaded;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_global(bytes.str(), fresh, pins));
    auto identity = loaded.identity_of(10);
    ZASSERT(identity);
    ZEXPECT(fresh.find(Spelling::absolute("/proj/0.h")) == Fid{identity->file});
    ZEXPECT(loaded.reference_count(10) == 1u);
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
    index::FileIndex file_index;
    file_index.relations[7].push_back({
        .kind = RelationKind::Definition,
        .range = {4, 5}
    });
    std::string shard;
    llvm::raw_string_ostream shard_os(shard);
    index::write_shard(file_index, {}, content, shard_os);
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
    // A file the database holds no shard for is asked once and stays absent.
    ZASSERT(project.shard(files.intern(Spelling::absolute("/proj/none.cpp"))) == nullptr);
    ZASSERT(project.shard(files.intern(Spelling::absolute("/proj/none.cpp"))) == nullptr);
}

ZEST_CASE(FanoutAcrossUnits) {
    // One hash in two places: a `static` in util.h, which a.cpp includes
    // and b.cpp reads through its precompiled header, and a namespace of
    // the same name nested in an anonymous namespace of other.h, which
    // only c.cpp includes. A walk steps from a.cpp to b.cpp through the
    // header they share, and never into c.cpp's files — in the writer and
    // in a reader of the persisted table.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, "");
    ZASSERT(db != nullptr);

    constexpr index::SymbolHash helper = 99;
    clice::FileTable pool;
    index::ProjectIndex writer;
    writer.global_generation = 3;
    auto util = pool.intern_version(pool.intern(Spelling::absolute("/proj/util.h")), 0xaaaa);
    auto other = pool.intern_version(pool.intern(Spelling::absolute("/proj/other.h")), 0xbbbb);
    llvm::SmallVector<Fid> units;
    for(auto name: {"/proj/a.cpp", "/proj/b.cpp", "/proj/c.cpp"}) {
        auto tu = pool.intern(Spelling::absolute(name));
        auto header = llvm::StringRef(name).ends_with("c.cpp") ? other : util;
        index::TUManifest manifest;
        manifest.global_gen = 3;
        manifest.tu_fv = pool.intern_version(tu, 0x1111);
        manifest.nodes = {
            {manifest.tu_fv.raw, ~0u, 1},
            {header.raw,         0,   1},
        };
        manifest.contributions = {
            {manifest.tu_fv, 1}
        };
        if(llvm::StringRef(name).ends_with("b.cpp")) {
            manifest.imports = {header};
        } else {
            manifest.contributions.emplace_back(header, 2);
        }
        manifest.local_fanout = {
            {.symbol = helper, .files = {manifest.tu_fv, header}}
        };
        writer.apply_manifest(pool, tu, std::move(manifest));
        units.push_back(tu);
    }

    auto check = [&](const index::ProjectIndex& index, clice::FileTable& table) {
        auto ids = [&](std::initializer_list<llvm::StringRef> names) {
            llvm::SmallVector<Fid> fids;
            for(auto name: names) {
                fids.push_back(table.intern(Spelling::absolute(name)));
            }
            llvm::sort(fids);
            return fids;
        };
        for(auto group: {ids({"/proj/a.cpp", "/proj/b.cpp", "/proj/util.h"}),
                         ids({"/proj/c.cpp", "/proj/other.h"})}) {
            for(auto start: group) {
                llvm::SmallVector<Fid> reached;
                index.each_fanout_file(helper, start, [&](Fid file) { reached.push_back(file); });
                llvm::sort(reached);
                ZEXPECT(reached == group);
            }
        }
    };
    check(writer, pool);

    std::vector<index::BlobDatabase::Blob> puts;
    for(auto tu: units) {
        auto exported = writer.export_manifest(writer.manifests.find(tu)->second);
        // The fanout names its files by persisted ids, as the nodes do.
        ZEXPECT(exported.local_fanout[0].files ==
                std::vector<VersionID>{exported.tu_fv, VersionID{exported.nodes[1].file}});
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(exported, os);
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
    check(reader, files);

    // A unit leaving takes its set along: c.cpp's files no longer answer.
    writer.remove_manifest(pool, units[2]);
    llvm::SmallVector<Fid> reached;
    writer.each_fanout_file(helper, pool.intern(Spelling::absolute("/proj/c.cpp")), [&](Fid file) {
        reached.push_back(file);
    });
    ZEXPECT(reached.size() == 1u);
    ZEXPECT(writer.fanout_sets.find(helper)->second.size() == 2u);
}

};  // ZEST_SUITE(ProjectIndex)

}  // namespace
}  // namespace clice::testing
