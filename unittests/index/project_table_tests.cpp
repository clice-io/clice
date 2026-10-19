module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_index;
import :index.tu_manifest;
import :support.archive;
import :unittests.utils.index_internals;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectTable) {

/// A project whose FileVersion for `path` is referenced by one manifest of
/// `tu` (so garbage collection keeps it) and whose only symbol references
/// `path` through `pool`.
index::ProjectIndex build_project(clice::FileTable& pool,
                                  llvm::StringRef path,
                                  llvm::StringRef tu) {
    index::ProjectIndex project{pool};
    auto path_id = pool.intern(Spelling::absolute(path));
    auto version = pool.intern_version(path_id, 0xabcd);

    index::TUManifest manifest;
    manifest.tu_version = pool.intern_version(pool.intern(Spelling::absolute(tu)), 0x1111);
    manifest.nodes = {
        {version.raw, ~0u, 3}
    };
    manifest.contributions = {
        {version, 777}
    };
    project.apply_manifest(pool.intern(Spelling::absolute(tu)), std::move(manifest));

    auto& symbol = project.edit(42);
    symbol.name = "sym";
    symbol.reference_files.add(path_id.raw);
    return project;
}

/// The files a symbol's bitmap names, as the pool's ids.
std::vector<std::uint32_t> reference_files(const index::ProjectIndex& project,
                                           index::SymbolHash hash) {
    std::vector<std::uint32_t> files;
    project.for_each_reference_file(hash, [&](Fid file) { files.push_back(file.raw); });
    return files;
}

ZEST_CASE(TableRoundTripRemap) {
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    IndexInternals::plant_generation(project, 9);
    auto tu = pool.intern(Spelling::absolute("/proj/tu.cpp"));
    auto manifest = *project.manifest(tu);
    manifest.table_generation = 9;
    project.apply_manifest(tu, std::move(manifest));

    auto buf = project.serialize_table();

    // The next session interns other paths first, so the same file gets a
    // different pool id; both the FileVersion table and the loaded bitmap
    // must follow the path, not the id.
    clice::FileTable fresh;
    fresh.intern(Spelling::absolute("/proj/opened-first.cpp"));
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.load_table(buf));

    auto id = fresh.find(Spelling::absolute("/proj/used.h"));
    ZASSERT(id);
    ZASSERT(llvm::is_contained(reference_files(loaded, 42), id->raw));
    ZASSERT(loaded.reference_count(42) == 1u);
    ZASSERT(fresh.versions.size() == pool.versions.size());
    ZASSERT(loaded.table_generation() == 9u);

    // The blob pins the TU's manifest at the generation it was saved under.
    auto tu_version = fresh.version_ids.find(
        {*fresh.find(Spelling::absolute("/proj/tu.cpp")), std::uint64_t(0x1111)});
    ZASSERT(tu_version != fresh.version_ids.end());
    ZASSERT(loaded.unit_count() == 1u);
    ZASSERT(IndexInternals::pinned_generation(loaded,
                                              *fresh.find(Spelling::absolute("/proj/tu.cpp"))) ==
            9u);

    ZASSERT(fresh.version_ids.contains({*id, std::uint64_t(0xabcd)}));
}

ZEST_CASE(TableRebaseKeepsChanges) {
    // The entries a write serialized read from the committed blob afterwards,
    // while entries changed across the write stay changed — and a write that
    // never committed gives its entries back.
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    auto other = pool.intern(Spelling::absolute("/proj/other.h"));

    auto first = project.serialize_table();
    project.edit(43).name = "late";
    project.edit(42).reference_files.add(other.raw);
    project.rebase(llvm::MemoryBuffer::getMemBufferCopy(first));
    // The unit written is merged still: the session's manifest stands for it.
    ZASSERT(project.unit_count() == 1u);
    ZASSERT(project.symbol_count() == 2u);
    ZASSERT(project.find_symbol(43)->name == "late");
    ZASSERT(project.reference_count(42) == 2u);

    auto second = project.serialize_table();
    project.restore_unwritten();
    ZASSERT(project.find_symbol(43)->name == "late");
    ZASSERT(project.reference_count(42) == 2u);

    // The base blob's path table is kept as it is, so a base entry's bitmap
    // survives the next write byte for byte.
    auto third = project.serialize_table();
    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.load_table(third));
    ZASSERT(loaded.symbol_count() == 2u);
    ZASSERT(loaded.reference_count(42) == 2u);
    ZASSERT(llvm::is_contained(reference_files(loaded, 42),
                               fresh.find(Spelling::absolute("/proj/other.h"))->raw));
}

ZEST_CASE(RebaseKeepsDroppedUnits) {
    // A unit dropped while its first table write was in flight is in the
    // written table: once that table is the base, the drop still stands.
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    auto first = project.serialize_table();
    index::IndexReport report;
    project.drop(pool.intern(Spelling::absolute("/proj/tu.cpp")), report);
    project.rebase(llvm::MemoryBuffer::getMemBufferCopy(first));
    ZASSERT(project.unit_count() == 0u);
    bool contributed = false;
    project.for_each_contribution(pool.intern(Spelling::absolute("/proj/used.h")),
                                  [&](Fid, std::uint64_t) { contributed = true; });
    ZASSERT(!contributed);
}

ZEST_CASE(TableCollectsGarbage) {
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    // Interned but referenced by no manifest — must not reach disk. The
    // shared table keeps it: other consumers may still anchor on it.
    auto dead_id = pool.intern(Spelling::absolute("/proj/dead.h"));
    pool.intern_version(dead_id, 0xdead);

    auto buf = project.serialize_table();
    ZASSERT(pool.version_ids.contains({dead_id, std::uint64_t(0xdead)}));

    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.load_table(buf));
    ZASSERT(!fresh.find(Spelling::absolute("/proj/dead.h")).has_value());
    ZASSERT(fresh.find(Spelling::absolute("/proj/used.h")));
}

ZEST_CASE(TableVersionGate) {
    // A table of another version is no table; an empty one of this version
    // is an empty index.
    clice::FileTable pool;
    index::ProjectIndex loaded{pool};

    index::ProjectTableBlob blob;
    ZASSERT(!loaded.load_table(archive::encode(blob)).has_value());
    blob.format_version = index::index_format_version;
    ZASSERT(loaded.load_table(archive::encode(blob)));
    ZASSERT(loaded.symbol_count() == 0u);
    ZASSERT(loaded.unit_count() == 0u);

    // Nor is an archive of another layout carrying the version.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    ZASSERT(
        !loaded.load_table(archive::encode(VersionOnly{index::index_format_version})).has_value());
    ZASSERT(!loaded.load_table("not an archive").has_value());
}

/// A file set as the writer stores it: up to two path ids raw as u32s, more
/// as a portable roaring image.
std::vector<std::uint8_t> file_set(const clice::Bitmap& ids) {
    std::vector<std::uint8_t> bytes;
    if(ids.cardinality() <= 2) {
        for(std::uint32_t id: ids) {
            auto* raw = reinterpret_cast<const std::uint8_t*>(&id);
            bytes.insert(bytes.end(), raw, raw + sizeof(id));
        }
        return bytes;
    }
    for(auto byte: index::write_bitmap(ids)) {
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    return bytes;
}

/// An empty project table blob of the current format.
index::ProjectTableBlob project_table_blob() {
    return {.format_version = index::index_format_version};
}

/// Append an entry, its parent outside the table; hashes must arrive
/// ascending, at most 255 of them.
void add_symbol(index::ProjectTableBlob& blob,
                std::uint64_t hash,
                llvm::StringRef name,
                llvm::ArrayRef<std::uint8_t> image,
                std::uint64_t parent = 0,
                std::uint32_t file = index::no_file) {
    if(parent != 0) {
        blob.symbol_far_parents.push_back(static_cast<std::uint32_t>(blob.symbol_hashes.size()));
        blob.symbol_far_parent_hashes.push_back(parent);
    }
    blob.symbol_hashes.push_back(hash);
    blob.symbol_names += name;
    blob.symbol_name_ends.push_back(static_cast<std::uint32_t>(blob.symbol_names.size()));
    blob.symbol_signature_ends.push_back(static_cast<std::uint32_t>(blob.symbol_signatures.size()));
    blob.symbol_parents.push_back(0);
    blob.symbol_kinds.push_back(0);
    blob.symbol_flags.push_back(0);
    blob.symbol_token_kinds.push_back(0);
    blob.symbol_token_modifiers.push_back(0);
    blob.symbol_files.push_back(static_cast<std::uint8_t>(file == index::no_file ? 0 : file + 1));
    blob.symbol_reference_counts.push_back(0);
    blob.symbol_bitmaps.insert(blob.symbol_bitmaps.end(), image.begin(), image.end());
    blob.symbol_bitmap_ends.push_back(static_cast<std::uint32_t>(blob.symbol_bitmaps.size()));
}

std::string encode(const index::ProjectTableBlob& blob) {
    return archive::encode(blob);
}

/// A path table covering ids 0..3, with the reference under test at 3.
std::vector<std::string> four_paths() {
    return {"/proj/0.h", "/proj/1.h", "/proj/2.h", "/proj/ref.h"};
}

ZEST_CASE(TableBitmapPayloadGate) {
    // A malformed reference bitmap must fail the whole load: normalized to
    // empty it would silently lose the symbol's reference files, with
    // nothing ever rebuilding them.
    clice::Bitmap bits;
    bits.add(3);
    auto layout = project_table_blob();
    layout.paths = four_paths();
    add_symbol(layout, 42, "sym", file_set(bits));

    clice::FileTable pool;
    auto valid = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(valid));
    ZASSERT(loaded.find_symbol(42));
    ZASSERT(llvm::is_contained(reference_files(loaded, 42),
                               pool.find(Spelling::absolute("/proj/ref.h"))->raw));

    // A malformed image after columns that decoded fine: the reject must
    // leave no partial state — file versions or symbols — that later
    // merges would build on and the next save persist.
    layout.next_version_id = 8;
    layout.versions = {
        {.id = 7, .path = "/proj/partial.h", .hash = 0x1}
    };
    add_symbol(layout, 43, "other", std::vector<std::uint8_t>{0xff, 0xff, 0xff});
    auto corrupt = encode(layout);
    clice::FileTable untouched;
    index::ProjectIndex rejecting{untouched};
    ZASSERT(!rejecting.load_table(corrupt).has_value());
    ZASSERT(rejecting.symbol_count() == 0u);
    ZASSERT(untouched.versions.empty());
    ZASSERT(!untouched.find(Spelling::absolute("/proj/partial.h")).has_value());
}

ZEST_CASE(UncoveredBitmapIdRejected) {
    // The writer emits a path-table entry for every id its bitmaps
    // reference; dropping an uncovered id would silently lose the symbol's
    // reference files while every manifest stays fresh.
    clice::Bitmap bits;
    bits.add(3);
    auto layout = project_table_blob();
    layout.paths = {"/proj/0.h"};
    add_symbol(layout, 42, "sym", file_set(bits));

    clice::FileTable pool;
    auto uncovered = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(uncovered).has_value());
    ZASSERT(loaded.symbol_count() == 0u);

    layout.paths = four_paths();
    auto covered = encode(layout);
    ZASSERT(loaded.load_table(covered));
    ZASSERT(loaded.find_symbol(42));
}

ZEST_CASE(TableDuplicateVersionsRejected) {
    // Version-table ids and (path, hash) pairs are both map keys in the
    // writer; a repeated id in particular would intern the earlier pair to
    // an id whose record names the later path, attributing contributions
    // to the wrong file.
    auto layout = project_table_blob();
    layout.next_version_id = 9;
    layout.versions = {
        {.id = 7, .path = "/proj/a.h", .hash = 0x1},
        {.id = 7, .path = "/proj/b.h", .hash = 0x2}
    };

    clice::FileTable pool;
    auto dup_id = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(dup_id).has_value());
    ZASSERT(pool.versions.empty());

    layout.versions = {
        {.id = 7, .path = "/proj/a.h", .hash = 0x1},
        {.id = 8, .path = "/proj/a.h", .hash = 0x1}
    };
    auto dup_pair = encode(layout);
    ZASSERT(!loaded.load_table(dup_pair).has_value());

    // The same path under two content hashes is the legitimate shape: two
    // observed versions of one file. Only the ids the blob carries map
    // into the table; the ones the writer garbage-collected map nowhere.
    layout.versions[1].hash = 0x2;
    auto distinct = encode(layout);
    ZASSERT(loaded.load_table(distinct));
    ZASSERT(pool.versions.size() == std::size_t(2));
    ZASSERT(loaded.runtime_version(7));
    ZASSERT(loaded.runtime_version(8));
    ZASSERT(!loaded.runtime_version(0).has_value());
}

ZEST_CASE(TableBadCounterRejected) {
    // Ids are handed out from the counter, so a legit writer's ids all sit
    // below it; a lagging counter would alias stored ids on the next
    // intern, and a sentinel one would insert a DenseMap reserved key.
    auto layout = project_table_blob();
    layout.next_version_id = 8;
    layout.versions = {
        {.id = 7, .path = "/proj/a.h", .hash = 0x1}
    };

    clice::FileTable pool;
    auto ahead = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(ahead));

    layout.next_version_id = 7;
    auto lagging = encode(layout);
    ZASSERT(!loaded.load_table(lagging).has_value());

    layout.next_version_id = std::numeric_limits<std::uint32_t>::max() - 1;
    layout.versions = {};
    auto reserved = encode(layout);
    ZASSERT(!loaded.load_table(reserved).has_value());

    // A garbage high-water mark far beyond any real lineage rejects.
    layout.next_version_id = 0xf0000000;
    auto oversized = encode(layout);
    ZASSERT(!loaded.load_table(oversized).has_value());
}

ZEST_CASE(TableRoundTripSymbolFacts) {
    clice::FileTable pool;
    index::ProjectIndex project{pool};
    auto file = pool.intern(Spelling::absolute("/proj/facts.h"));
    auto& parent = project.edit(7);
    parent.name = "ns";
    parent.kind = SymbolKind::Namespace;
    auto& symbol = project.edit(42);
    symbol.name = "Box";
    symbol.args = "<int>";
    symbol.parent = 7;
    symbol.kind = SymbolKind::Struct;
    symbol.flags = index::SymbolFlags::HasDefinition | index::SymbolFlags::Specialization;
    symbol.file = file.raw;

    auto buf = project.serialize_table();

    // The file column follows the path across pools like the bitmaps do.
    clice::FileTable fresh;
    fresh.intern(Spelling::absolute("/proj/opened-first.cpp"));
    index::ProjectIndex loaded{fresh};
    ZASSERT(loaded.load_table(buf));
    auto restored = loaded.find_symbol(42);
    ZASSERT(restored);
    ZASSERT(restored->name == "Box");
    ZASSERT(restored->args == "<int>");
    ZASSERT(restored->parent == 7u);
    ZASSERT(static_cast<std::uint16_t>(restored->flags) ==
            static_cast<std::uint16_t>(symbol.flags));
    auto moved = fresh.find(Spelling::absolute("/proj/facts.h"));
    ZASSERT(moved);
    ZASSERT(restored->file == moved->raw);
    ZASSERT(loaded.find_symbol(7)->file == index::no_file);
}

ZEST_CASE(UncoveredFileIdRejected) {
    // A file id the path table does not cover would resolve to whatever
    // this session interned at that id: reject it like an uncovered
    // bitmap id.
    auto layout = project_table_blob();
    add_symbol(layout, 42, "sym", file_set(clice::Bitmap{}));

    clice::FileTable pool;
    auto control = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(control));

    layout.symbol_files = {6};
    auto uncovered = encode(layout);
    index::ProjectIndex rejecting{pool};
    ZASSERT(!rejecting.load_table(uncovered).has_value());
    ZASSERT(rejecting.symbol_count() == 0u);
}

ZEST_CASE(TableSparseColumnsRejected) {
    // The sparse columns name symbols by position: one past the table or
    // out of order would hand a symbol another's arguments or parent.
    auto layout = project_table_blob();
    add_symbol(layout, 42, "a", file_set(clice::Bitmap{}));
    add_symbol(layout, 43, "b", file_set(clice::Bitmap{}));
    layout.symbol_args_entries = {1};
    layout.symbol_args = "<int>";
    layout.symbol_args_ends = {5};

    clice::FileTable pool;
    auto loads = [&] {
        auto bytes = encode(layout);
        index::ProjectIndex loaded{pool};
        return loaded.load_table(bytes).has_value();
    };
    ZASSERT(loads());
    layout.symbol_args_entries = {2};
    ZEXPECT(!loads());
    layout.symbol_args_entries = {1};

    layout.symbol_far_parents = {1, 0};
    layout.symbol_far_parent_hashes = {7, 8};
    ZEXPECT(!loads());
    layout.symbol_far_parents = {0, 1};
    ZEXPECT(loads());
}

ZEST_CASE(TableDuplicateSymbolRejected) {
    // Entries are looked up by binary search over the hash column, so it
    // must ascend strictly; a repeated hash would let one entry shadow the
    // other's identity and reference bitmap while every manifest still
    // loads as fresh.
    clice::Bitmap bits;
    bits.add(3);
    auto layout = project_table_blob();
    layout.paths = four_paths();
    add_symbol(layout, 42, "sym", file_set(bits));
    add_symbol(layout, 42, "impostor", file_set(bits));

    clice::FileTable pool;
    auto dup = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(dup).has_value());
    ZASSERT(loaded.symbol_count() == 0u);

    layout.symbol_hashes = {42, 43};
    auto distinct = encode(layout);
    ZASSERT(loaded.load_table(distinct));
    ZASSERT(loaded.symbol_count() == std::size_t(2));

    layout.symbol_hashes = {43, 42};
    auto descending = encode(layout);
    ZASSERT(!loaded.load_table(descending).has_value());
}

ZEST_CASE(TableTokenKindRejected) {
    // A symbol's token kind indexes the legend's tables: one past the enum
    // marks a corrupt blob.
    auto layout = project_table_blob();
    layout.paths = four_paths();
    add_symbol(layout, 42, "sym", file_set(clice::Bitmap{}));
    layout.symbol_token_kinds = {200};

    clice::FileTable pool;
    auto corrupt = encode(layout);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(corrupt).has_value());

    layout.symbol_token_kinds = {0};
    auto valid = encode(layout);
    ZASSERT(loaded.load_table(valid));
}

ZEST_CASE(TableReservedKeysRejected) {
    // The two DenseMap sentinel key values can never sit in the in-memory
    // tables, so the writer can never emit them; a blob carrying one is
    // corrupt, and inserting it would corrupt (or assert in) the loader's
    // own containers.
    clice::FileTable pool;
    index::ProjectIndex loaded{pool};

    {
        auto layout = project_table_blob();
        layout.versions = {
            {.id = 0xffffffffu, .path = "/proj/a.h", .hash = 0x1}
        };
        auto bytes = encode(layout);
        ZASSERT(!loaded.load_table(bytes).has_value());
    }
    {
        clice::Bitmap bits;
        bits.add(3);
        auto layout = project_table_blob();
        layout.paths = four_paths();
        add_symbol(layout, ~std::uint64_t(0), "sym", file_set(bits));
        auto bytes = encode(layout);
        ZASSERT(!loaded.load_table(bytes).has_value());
    }
    {
        auto layout = project_table_blob();
        add_symbol(layout, 42, "sym", file_set(clice::Bitmap{}), ~std::uint64_t(0));
        auto bytes = encode(layout);
        ZASSERT(!loaded.load_table(bytes).has_value());
    }
    {
        auto layout = project_table_blob();
        layout.paths = {""};
        auto bytes = encode(layout);
        ZASSERT(!loaded.load_table(bytes).has_value());
    }
    ZASSERT(pool.versions.empty());
    ZASSERT(loaded.symbol_count() == 0u);
}

ZEST_CASE(UnknownFileVersionsDetected) {
    auto layout = project_table_blob();
    layout.next_version_id = 4;
    layout.versions = {
        {.id = 3, .path = "/proj/a.h", .hash = 0x1}
    };
    auto bytes = encode(layout);

    clice::FileTable pool;
    pool.intern_version(pool.intern(Spelling::absolute("/proj/opened-first.cpp")), 0x9);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(bytes));
    auto known =
        pool.version_ids.find({*pool.find(Spelling::absolute("/proj/a.h")), std::uint64_t(0x1)})
            ->second;

    index::TUManifest manifest;
    manifest.tu_version = VersionID{3};
    manifest.nodes = {
        {3, ~0u, 1}
    };
    manifest.contributions = {
        {VersionID{3}, 7}
    };
    manifest.shared_locals = {
        {.symbol = 5, .files = {VersionID{3}}}
    };
    auto imported = manifest;
    ZASSERT(loaded.import_manifest(imported));
    ZASSERT(imported.tu_version == known);
    ZASSERT(imported.nodes[0].file == known.raw);
    ZASSERT(imported.contributions[0].first == known);
    ZASSERT(imported.shared_locals[0].files[0] == known);

    auto widened = manifest;
    widened.shared_locals[0].files.push_back(VersionID{2});
    ZASSERT(!loaded.import_manifest(widened));

    manifest.nodes.push_back({2, ~0u, 2});
    ZASSERT(!loaded.import_manifest(manifest));
    manifest.nodes.back().file = ~0u;
    ZASSERT(!loaded.import_manifest(manifest));
}

ZEST_CASE(SharedTableLineages) {
    // Two projects' indexes over one file table: each keeps its own
    // persisted ids, so the second loads beside the first and writes its
    // manifests back under the ids it read them with.
    clice::FileTable first_pool;
    auto first = build_project(first_pool, "/lib/used.h", "/lib/tu.cpp");
    auto first_bytes = first.serialize_table();

    clice::FileTable second_pool;
    second_pool.intern_version(second_pool.intern(Spelling::absolute("/app/pad.cpp")), 0x5);
    auto second = build_project(second_pool, "/lib/used.h", "/app/tu.cpp");
    auto written = second.manifest(second_pool.intern(Spelling::absolute("/app/tu.cpp")));
    ZASSERT(written);
    auto persisted = second.export_manifest(*written);
    auto second_bytes = second.serialize_table();

    clice::FileTable shared;
    index::ProjectIndex first_loaded{shared};
    index::ProjectIndex second_loaded{shared};
    ZASSERT(first_loaded.load_table(first_bytes));
    ZASSERT(second_loaded.load_table(second_bytes));

    // The header both index is one version of the shared table.
    ZASSERT(shared.versions.size() == std::size_t(3));
    auto header = shared.version_ids.find(
        {*shared.find(Spelling::absolute("/lib/used.h")), std::uint64_t(0xabcd)});
    ZASSERT(header != shared.version_ids.end());

    auto imported = persisted;
    ZASSERT(second_loaded.import_manifest(imported));
    ZASSERT(shared.version(imported.tu_version).fid ==
            *shared.find(Spelling::absolute("/app/tu.cpp")));
    ZASSERT(imported.contributions[0].first == header->second);
    ZASSERT(second_loaded.unit_count() == 1u);
    ZASSERT(second_loaded.has_manifest(shared.version(imported.tu_version).fid));
    ZASSERT(second_loaded.export_manifest(imported) == persisted);
}

};  // ZEST_SUITE(ProjectTable)

}  // namespace
}  // namespace clice::testing
