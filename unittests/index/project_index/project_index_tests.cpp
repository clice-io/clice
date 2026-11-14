module clice;

import :index.database;
import :index.indexing;
import :index.project_index;
import :index.shard;
import :index.indexing.tu_index;
import :support.text;
import :unittests.utils.index_internals;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :unittests.utils.tu_index_blob;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.path;

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

/// The files of a symbol's record, as the pool's ids.
std::vector<std::uint32_t> files_of(const index::ProjectIndex& project,
                                    index::SymbolHash hash,
                                    index::SymbolFiles which = index::SymbolFiles::Naming) {
    std::vector<std::uint32_t> files;
    project.for_each_file(hash, which, [&](Fid file) { files.push_back(file.raw); });
    return files;
}

/// The TU-local id -> pool id mapping, as merge() computes it.
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
    ZASSERT(project.merge(view.bytes(), {}).has_value());

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
    ZASSERT(project.merge(view.bytes(), {}).has_value());

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

    // The defining unit's file defines it, and its bits join the union.
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
    ZASSERT(project.merge(definer_view.bytes(), {}).has_value());

    auto defined = project.find_symbol(hash);
    ZASSERT(defined);
    ZASSERT(index::has_flag(defined->flags, index::SymbolFlags::HasDefinition));
    ZASSERT(index::has_flag(defined->flags, index::SymbolFlags::Deprecated));
    auto definition = pool.find(Spelling::absolute(definer_view.path(definer_view.focus_file())));
    ZASSERT(definition);
    ZASSERT(defined->file == definition->raw);
    ZASSERT(project.reference_count(hash) == 3u);
    ZASSERT(files_of(project, hash, index::SymbolFiles::Defining) == std::vector{definition->raw});

    // The definition leaves the unit: its file names the symbol no more,
    // and the record follows the shard.
    Tester emptied;
    emptied.add_focus("lib.cpp", "int unrelated = 0;\n");
    emptied.kind = CompilationKind::Whole;
    ZASSERT(emptied.compile());
    auto emptied_bytes = index::build_tu_index(*emptied.unit);
    ZASSERT(project.merge(as_chars(emptied_bytes), {}).has_value());
    auto declared_again = project.find_symbol(hash);
    ZASSERT(declared_again);
    ZASSERT(!index::has_flag(declared_again->flags, index::SymbolFlags::HasDefinition));
    ZASSERT(declared_again->file == header->raw);
    ZASSERT(project.reference_count(hash) == 2u);
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
        IndexInternals::merge_facts(project, first_view);
        IndexInternals::merge_facts(project, second_view);
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
    IndexInternals::merge_facts(project, before_view);
    IndexInternals::merge_facts(project, after_view);
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
        IndexInternals::merge_facts(project, first_view);
        // The shards of the defining unit say its file defines the symbol.
        if(first == &defines_bytes) {
            auto file =
                IndexInternals::path_id(project, pool.intern(Spelling::absolute("/proj/main.cpp")));
            auto& defined = IndexInternals::symbol(project, 42);
            defined.declaration_files.add(file);
            defined.definition_files.add(file);
        }
        IndexInternals::merge_facts(project, second_view);
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
        IndexInternals::merge_facts(project, first_view);
        IndexInternals::merge_facts(project, second_view);
        ZASSERT(project.find_symbol(42)->token_class.kind == SemanticTokenKind::Class);
    }
}

ZEST_CASE(DefinitionFilesFromShards) {
    // A weak and a strong definition, each a unit's: both files define the
    // symbol, whichever merges first, and a unit merged again changes
    // nothing.
    auto unit_defining = [](llvm::StringRef name, llvm::StringRef text) {
        Tester tester;
        tester.add_focus(name, text);
        tester.kind = CompilationKind::Whole;
        ZASSERT(tester.compile("-std=c17"));
        return index::build_tu_index(*tester.unit);
    };
    auto weak = unit_defining("weak.c", "__attribute__((weak)) void father(void) {}\n");
    auto strong = unit_defining("main.c", "void father(void) {}\n");
    clice::FileTable pool;
    auto names = [&](index::ProjectIndex& project, index::SymbolHash hash) {
        std::vector<std::string> files;
        project.for_each_file(hash, index::SymbolFiles::Defining, [&](Fid file) {
            files.push_back(std::string(path::filename(pool.resolve(file))));
        });
        ranges::sort(files);
        return files;
    };
    for(auto [first, second]: {
            std::pair{&weak,   &strong},
            std::pair{&strong, &weak  }
    }) {
        index::ProjectIndex project{pool};
        ZASSERT(project.merge(as_chars(*first), {}).has_value());
        ZASSERT(project.merge(as_chars(*second), {}).has_value());
        ZASSERT(project.merge(as_chars(*first), {}).has_value());
        auto father = find_symbol(project, "father");
        ZASSERT(father != 0);
        ZEXPECT(names(project, father) == std::vector<std::string>{"main.c", "weak.c"});
    }
}

ZEST_CASE(TentativeDefinitionsBothFiles) {
    // A C header's tentative definition and the source's own both define
    // the variable in the one unit.
    add_file("counter.h", "int counter;\n");
    add_focus("main.c", "#include \"counter.h\"\nint counter = 1;\n");
    kind = CompilationKind::Whole;
    ZASSERT(compile("-std=c17"));

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto view = build_view();
    ZASSERT(view.loaded());
    ZASSERT(project.merge(view.bytes(), {}).has_value());
    auto counter = find_symbol(project, "counter");
    ZASSERT(counter != 0);
    std::vector<std::string> files;
    project.for_each_file(counter, index::SymbolFiles::Defining, [&](Fid file) {
        files.push_back(std::string(path::filename(pool.resolve(file))));
    });
    ranges::sort(files);
    ZEXPECT(files == std::vector<std::string>{"counter.h", "main.c"});
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
    auto db = index::open_database(*store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto view = build_view();
    ZASSERT(view.loaded());
    ZASSERT(project.merge(view.bytes(), {}).has_value());
    auto focus = intern_paths(view, pool)[view.focus_file()];
    commit(project, *db);

    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.open(*db) == index::LoadResult::Loaded);
    auto symbol = find_symbol(loaded, "global_value");
    ZASSERT(symbol != 0);
    auto fresh_id = fresh.find(Spelling::absolute(pool.resolve(focus)));
    ZASSERT(fresh_id);
    ZASSERT(ranges::contains(files_of(loaded, symbol), fresh_id->raw));
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
    auto db = index::open_database(*store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    std::vector<std::uint32_t> paths;
    for(int i = 0; i < 4; i += 1) {
        auto file = pool.intern(Spelling::absolute(std::format("/proj/{}.h", i)));
        paths.push_back(IndexInternals::path_id(project, file));
    }
    auto& scope = IndexInternals::symbol(project, 10);
    scope.name = "ns";
    scope.kind = SymbolKind::Namespace;
    scope.reference_files.add(paths[0]);
    auto& member = IndexInternals::symbol(project, 20);
    member.name = "f";
    member.args = "<int>";
    member.parent = 10;
    member.declaration_files.add(paths[1]);
    member.reference_files.add(paths[1]);
    member.reference_files.add(paths[2]);
    auto& outsider = IndexInternals::symbol(project, 30);
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
        auto referencing = files_of(index, 30);
        ranges::sort(referencing);
        std::vector<std::uint32_t> expected{id_of("/proj/0.h"),
                                            id_of("/proj/1.h"),
                                            id_of("/proj/2.h"),
                                            id_of("/proj/3.h")};
        ranges::sort(expected);
        ZEXPECT(referencing == expected);
        ZEXPECT(files_of(index, 30, index::SymbolFiles::Instantiating) ==
                std::vector{id_of("/proj/3.h")});
        ZEXPECT(files_of(index, 20, index::SymbolFiles::Instantiating).empty());
    };
    check(project, pool);

    commit(project, *db);
    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.open(*db) == index::LoadResult::Loaded);
    check(loaded, fresh);
}

ZEST_CASE(LazyShardsStayPut) {
    // A reader fetches shards while a query still holds the ones it read
    // first: the fan-out of a position query resolves other files between
    // taking a shard and reading it again.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    clice::FileTable writer_files;
    index::ProjectIndex writer{writer_files};
    IndexInternals::symbol(writer, 7).name = "x";
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

    ZASSERT(project.open(*db) == index::LoadResult::Loaded);
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

ZEST_CASE(LocalSymbolAcrossFiles) {
    // A header's `static` function is one symbol in every file naming it:
    // its record lists them once a unit names it in several, in the writer
    // and in a reader of the persisted records, and a file whose shard
    // names it no more leaves.
    TempDir tmp;
    auto store = CacheStore::open(tmp.path("lmdb"), 1, false);
    ZASSERT(store);
    auto db = index::open_database(*store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    auto unit_of = [](llvm::StringRef name, llvm::StringRef text) {
        Tester tester;
        tester.add_file("util.h", "static int helper() { return 1; }\n");
        tester.add_focus(name, text);
        tester.kind = CompilationKind::Whole;
        ZASSERT(tester.compile());
        return index::build_tu_index(*tester.unit);
    };
    llvm::StringRef caller = "#include \"util.h\"\nint call() { return helper(); }\n";
    auto a = unit_of("a.cpp", caller);
    auto b = unit_of("b.cpp", caller);

    auto view = index::TUIndex::from_bytes(as_chars(a));
    ZASSERT(view.loaded());
    index::SymbolHash helper = 0;
    view.for_each_symbol([&](index::SymbolHash hash,
                             const index::SymbolView& symbol,
                             llvm::ArrayRef<std::uint32_t>) {
        if(symbol.name == "helper") {
            helper = hash;
        }
        return helper == 0;
    });
    ZASSERT(helper != 0);

    clice::FileTable pool;
    index::ProjectIndex writer{pool};
    ZASSERT(writer.merge(as_chars(a), {}).has_value());
    ZASSERT(writer.merge(as_chars(b), {}).has_value());
    ZEXPECT(!writer.find_symbol(helper));

    auto names =
        [&](const index::ProjectIndex& index, clice::FileTable& table, index::SymbolFiles which) {
            std::vector<std::string> files;
            index.for_each_file(helper, which, [&](Fid file) {
                files.push_back(std::string(path::filename(table.resolve(file))));
            });
            ranges::sort(files);
            return files;
        };
    auto everywhere = std::vector<std::string>{"a.cpp", "b.cpp", "util.h"};
    ZEXPECT(names(writer, pool, index::SymbolFiles::Naming) == everywhere);
    ZEXPECT(names(writer, pool, index::SymbolFiles::Defining) ==
            std::vector<std::string>{"util.h"});

    commit(writer, *db);
    clice::FileTable files;
    index::ProjectIndex reader{files};
    ZASSERT(reader.open(*db) == index::LoadResult::Loaded);
    ZEXPECT(names(reader, files, index::SymbolFiles::Naming) == everywhere);

    auto quiet = unit_of("b.cpp", "#include \"util.h\"\nint call() { return 0; }\n");
    ZASSERT(writer.merge(as_chars(quiet), {}).has_value());
    ZEXPECT(names(writer, pool, index::SymbolFiles::Naming) ==
            std::vector<std::string>{"a.cpp", "util.h"});
}

ZEST_CASE(LocalBehindPrecompiledHeader) {
    // A unit reading util.h through its precompiled header names the
    // header's `static` function and writes none of the header's entries:
    // the function's record lists util.h once another unit's entries of it
    // are stored, before the first unit merges or after.
    auto unit_of = [](llvm::StringRef name, llvm::StringRef text, bool precompiled) {
        Tester tester;
        tester.add_file("util.h", "static int helper() { return 1; }\n");
        tester.add_focus(name, text);
        if(precompiled) {
            ZASSERT(tester.compile_with_pch());
        } else {
            tester.kind = CompilationKind::Whole;
            ZASSERT(tester.compile());
        }
        return index::build_tu_index(*tester.unit);
    };
    auto consumer =
        unit_of("main.cpp", "#include \"util.h\"\nint call() { return helper(); }\n", true);
    auto other = unit_of("other.cpp", "#include \"util.h\"\nint other() { return 0; }\n", false);

    auto view = index::TUIndex::from_bytes(as_chars(consumer));
    ZASSERT(view.loaded());
    index::SymbolHash helper = 0;
    view.for_each_symbol([&](index::SymbolHash hash,
                             const index::SymbolView& symbol,
                             llvm::ArrayRef<std::uint32_t>) {
        if(symbol.name == "helper") {
            helper = hash;
        }
        return helper == 0;
    });
    ZASSERT(helper != 0);

    for(auto [first, second]: {
            std::pair{&consumer, &other   },
            std::pair{&other,    &consumer}
    }) {
        clice::FileTable pool;
        index::ProjectIndex project{pool};
        ZASSERT(project.merge(as_chars(*first), {}).has_value());
        ZASSERT(project.merge(as_chars(*second), {}).has_value());
        auto names = [&](index::SymbolFiles which) {
            std::vector<std::string> files;
            project.for_each_file(helper, which, [&](Fid file) {
                files.push_back(std::string(path::filename(pool.resolve(file))));
            });
            ranges::sort(files);
            return files;
        };
        ZEXPECT(names(index::SymbolFiles::Naming) ==
                std::vector<std::string>{"main.cpp", "util.h"});
        ZEXPECT(names(index::SymbolFiles::Defining) == std::vector<std::string>{"util.h"});
    }
}

ZEST_CASE(SymbolMovesBetweenFiles) {
    // A function moved from one header of a unit to the other leaves one
    // shard and enters the other in the same merge: its record follows,
    // whichever shard the merge replaces first.
    llvm::StringRef function = "inline int moved() { return 0; }\n";
    auto unit_with = [&](bool in_first) {
        Tester tester;
        tester.add_file("first.h", std::format("extern int first;\n{}", in_first ? function : ""));
        tester.add_file("second.h",
                        std::format("extern int second;\n{}", in_first ? "" : function));
        tester.add_focus("main.cpp", "#include \"first.h\"\n#include \"second.h\"\n");
        tester.kind = CompilationKind::Whole;
        ZASSERT(tester.compile());
        return index::build_tu_index(*tester.unit);
    };

    clice::FileTable pool;
    index::ProjectIndex project{pool};
    for(bool in_first: {true, false, true}) {
        ZASSERT(project.merge(as_chars(unit_with(in_first)), {}).has_value());
        auto moved = find_symbol(project, "moved");
        ZASSERT(moved != 0);
        std::vector<std::string> files;
        project.for_each_file(moved, index::SymbolFiles::Naming, [&](Fid file) {
            files.push_back(std::string(path::filename(pool.resolve(file))));
        });
        ZEXPECT(files == std::vector<std::string>{in_first ? "first.h" : "second.h"});
    }
}

};  // ZEST_SUITE(ProjectIndex)

}  // namespace
}  // namespace clice::testing
