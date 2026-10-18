module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_index;
import :index.tu_manifest;
import :unittests.utils.index_planter;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectTable) {

llvm::StringRef bytes_of(const std::vector<std::uint8_t>& blob) {
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

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
    IndexPlanter::plant_generation(project, 9);
    auto tu = pool.intern(Spelling::absolute("/proj/tu.cpp"));
    auto manifest = *project.held_manifest(tu);
    manifest.table_generation = 9;
    project.apply_manifest(tu, std::move(manifest));

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_table(os);

    // The next session interns other paths first, so the same file gets a
    // different pool id; both the FileVersion table and the loaded bitmap
    // must follow the path, not the id.
    clice::FileTable fresh;
    fresh.intern(Spelling::absolute("/proj/opened-first.cpp"));
    index::ProjectIndex loaded{fresh};
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_table(buf.str(), pins));

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
    ZASSERT(pins.size() == std::size_t(1));
    ZASSERT(pins.find(tu_version->second)->second == 9u);

    ZASSERT(fresh.version_ids.contains({*id, std::uint64_t(0xabcd)}));
}

ZEST_CASE(TableRebaseKeepsChanges) {
    // The entries a write serialized read from the committed blob afterwards,
    // while entries changed across the write stay changed — and a write that
    // never committed gives its entries back.
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    auto other = pool.intern(Spelling::absolute("/proj/other.h"));

    std::string first;
    llvm::raw_string_ostream first_os(first);
    project.serialize_table(first_os);
    project.edit(43).name = "late";
    project.edit(42).reference_files.add(other.raw);
    project.rebase(llvm::MemoryBuffer::getMemBufferCopy(first));
    ZASSERT(project.symbol_count() == 2u);
    ZASSERT(project.find_symbol(43)->name == "late");
    ZASSERT(project.reference_count(42) == 2u);

    std::string second;
    llvm::raw_string_ostream second_os(second);
    project.serialize_table(second_os);
    project.restore_unwritten();
    ZASSERT(project.find_symbol(43)->name == "late");
    ZASSERT(project.reference_count(42) == 2u);

    // The base blob's path table is kept as it is, so a base entry's bitmap
    // survives the next write byte for byte.
    std::string third;
    llvm::raw_string_ostream third_os(third);
    project.serialize_table(third_os);
    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_table(third, pins));
    ZASSERT(loaded.symbol_count() == 2u);
    ZASSERT(loaded.reference_count(42) == 2u);
    ZASSERT(llvm::is_contained(reference_files(loaded, 42),
                               fresh.find(Spelling::absolute("/proj/other.h"))->raw));
}

ZEST_CASE(TableCollectsGarbage) {
    clice::FileTable pool;
    auto project = build_project(pool, "/proj/used.h", "/proj/tu.cpp");
    // Interned but referenced by no manifest — must not reach disk. The
    // shared table keeps it: other consumers may still anchor on it.
    auto dead_id = pool.intern(Spelling::absolute("/proj/dead.h"));
    pool.intern_version(dead_id, 0xdead);

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_table(os);
    ZASSERT(pool.version_ids.contains({dead_id, std::uint64_t(0xdead)}));

    clice::FileTable fresh;
    index::ProjectIndex loaded{fresh};
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_table(buf.str(), pins));
    ZASSERT(!fresh.find(Spelling::absolute("/proj/dead.h")).has_value());
    ZASSERT(fresh.find(Spelling::absolute("/proj/used.h")));
}

ZEST_CASE(TableVersionGate) {
    // Only the version slot is written: every other field reads back absent,
    // which is structurally valid — the result must hinge on the value.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    clice::FileTable pool;
    index::ProjectIndex loaded{pool};
    llvm::DenseMap<VersionID, std::uint64_t> pins;

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{});
    ZASSERT(stale);
    ZASSERT(!loaded.load_table(bytes_of(*stale), pins).has_value());

    auto current = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ZASSERT(current);
    ZASSERT(loaded.load_table(bytes_of(*current), pins));
    ZASSERT(loaded.symbol_count() == 0u);
    ZASSERT(pins.empty());

    ZASSERT(!loaded.load_table("not a flatbuffer", pins).has_value());
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

auto encode(const index::ProjectTableBlob& blob) {
    return kota::codec::fbs::to_bytes(blob);
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
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto valid = encode(layout);
    ZASSERT(valid);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(bytes_of(*valid), pins));
    ZASSERT(loaded.find_symbol(42));
    ZASSERT(llvm::is_contained(reference_files(loaded, 42),
                               pool.find(Spelling::absolute("/proj/ref.h"))->raw));

    // A malformed image after columns that decoded fine: the reject must
    // leave no partial state — file versions or symbols — that later
    // merges would build on and the next save persist.
    layout.next_version_id = 8;
    layout.version_ids = {7};
    layout.version_paths = {"/proj/partial.h"};
    layout.version_hashes = {0x1};
    add_symbol(layout, 43, "other", std::vector<std::uint8_t>{0xff, 0xff, 0xff});
    auto corrupt = encode(layout);
    ZASSERT(corrupt);
    clice::FileTable untouched;
    index::ProjectIndex rejecting{untouched};
    ZASSERT(!rejecting.load_table(bytes_of(*corrupt), pins).has_value());
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
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto uncovered = encode(layout);
    ZASSERT(uncovered);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(bytes_of(*uncovered), pins).has_value());
    ZASSERT(loaded.symbol_count() == 0u);

    layout.paths = four_paths();
    auto covered = encode(layout);
    ZASSERT(covered);
    ZASSERT(loaded.load_table(bytes_of(*covered), pins));
    ZASSERT(loaded.find_symbol(42));
}

ZEST_CASE(TableDuplicateVersionsRejected) {
    // Version-table ids and (path, hash) pairs are both map keys in the
    // writer; a repeated id in particular would intern the earlier pair to
    // an id whose record names the later path, attributing contributions
    // to the wrong file.
    auto layout = project_table_blob();
    layout.next_version_id = 9;
    layout.version_ids = {7, 7};
    layout.version_paths = {"/proj/a.h", "/proj/b.h"};
    layout.version_hashes = {0x1, 0x2};

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto dup_id = encode(layout);
    ZASSERT(dup_id);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(bytes_of(*dup_id), pins).has_value());
    ZASSERT(pool.versions.empty());

    layout.version_ids = {7, 8};
    layout.version_paths = {"/proj/a.h", "/proj/a.h"};
    layout.version_hashes = {0x1, 0x1};
    auto dup_pair = encode(layout);
    ZASSERT(dup_pair);
    ZASSERT(!loaded.load_table(bytes_of(*dup_pair), pins).has_value());

    // The same path under two content hashes is the legitimate shape: two
    // observed versions of one file. Only the ids the blob carries map
    // into the table; the ones the writer garbage-collected map nowhere.
    layout.version_hashes = {0x1, 0x2};
    auto distinct = encode(layout);
    ZASSERT(distinct);
    ZASSERT(loaded.load_table(bytes_of(*distinct), pins));
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
    layout.version_ids = {7};
    layout.version_paths = {"/proj/a.h"};
    layout.version_hashes = {0x1};

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto ahead = encode(layout);
    ZASSERT(ahead);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(bytes_of(*ahead), pins));

    layout.next_version_id = 7;
    auto lagging = encode(layout);
    ZASSERT(lagging);
    ZASSERT(!loaded.load_table(bytes_of(*lagging), pins).has_value());

    layout.next_version_id = std::numeric_limits<std::uint32_t>::max() - 1;
    layout.version_ids = {};
    layout.version_paths = {};
    layout.version_hashes = {};
    auto reserved = encode(layout);
    ZASSERT(reserved);
    ZASSERT(!loaded.load_table(bytes_of(*reserved), pins).has_value());

    // A garbage high-water mark far beyond any real lineage rejects.
    layout.next_version_id = 0xf0000000;
    auto oversized = encode(layout);
    ZASSERT(oversized);
    ZASSERT(!loaded.load_table(bytes_of(*oversized), pins).has_value());
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

    llvm::SmallString<1024> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_table(os);

    // The file column follows the path across pools like the bitmaps do.
    clice::FileTable fresh;
    fresh.intern(Spelling::absolute("/proj/opened-first.cpp"));
    index::ProjectIndex loaded{fresh};
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_table(buf.str(), pins));
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
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto control = encode(layout);
    ZASSERT(control);
    index::ProjectIndex loaded{pool};
    ZASSERT(loaded.load_table(bytes_of(*control), pins));

    layout.symbol_files = {6};
    auto uncovered = encode(layout);
    ZASSERT(uncovered);
    index::ProjectIndex rejecting{pool};
    ZASSERT(!rejecting.load_table(bytes_of(*uncovered), pins).has_value());
    ZASSERT(rejecting.symbol_count() == 0u);
}

ZEST_CASE(SharedLocalFilesRejected) {
    // A reader binary-searches the symbols for the file sets the shared locals
    // list: symbols out of order, or ends that misplace a symbol's sets or
    // a set's files, must not load.
    auto layout = project_table_blob();
    layout.paths = {"/proj/a.h", "/proj/a.cpp"};
    layout.shared_local_symbols = {5, 9};
    layout.shared_local_set_ends = {1, 2};
    layout.shared_local_files = {0, 0, 0, 0, 1, 0, 0, 0};
    layout.shared_local_file_ends = {4, 8};

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto loads = [&](const index::ProjectTableBlob& blob) {
        auto bytes = encode(blob);
        ZASSERT(bytes);
        index::ProjectIndex loaded{pool};
        return loaded.load_table(bytes_of(*bytes), pins).has_value();
    };
    ZASSERT(loads(layout));

    auto unordered = layout;
    unordered.shared_local_symbols = {9, 5};
    ZEXPECT(!loads(unordered));

    auto misplaced = layout;
    misplaced.shared_local_file_ends = {4, 7};
    ZEXPECT(!loads(misplaced));

    auto overrun = layout;
    overrun.shared_local_set_ends = {1, 3};
    ZEXPECT(!loads(overrun));
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
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto loads = [&] {
        auto bytes = encode(layout);
        index::ProjectIndex loaded{pool};
        return bytes && loaded.load_table(bytes_of(*bytes), pins).has_value();
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
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto dup = encode(layout);
    ZASSERT(dup);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(bytes_of(*dup), pins).has_value());
    ZASSERT(loaded.symbol_count() == 0u);

    layout.symbol_hashes = {42, 43};
    auto distinct = encode(layout);
    ZASSERT(distinct);
    ZASSERT(loaded.load_table(bytes_of(*distinct), pins));
    ZASSERT(loaded.symbol_count() == std::size_t(2));

    layout.symbol_hashes = {43, 42};
    auto descending = encode(layout);
    ZASSERT(descending);
    ZASSERT(!loaded.load_table(bytes_of(*descending), pins).has_value());
}

ZEST_CASE(TableTokenKindRejected) {
    // A symbol's token kind indexes the legend's tables: one past the enum
    // marks a corrupt blob.
    auto layout = project_table_blob();
    layout.paths = four_paths();
    add_symbol(layout, 42, "sym", file_set(clice::Bitmap{}));
    layout.symbol_token_kinds = {200};

    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    auto corrupt = encode(layout);
    ZASSERT(corrupt);
    index::ProjectIndex loaded{pool};
    ZASSERT(!loaded.load_table(bytes_of(*corrupt), pins).has_value());

    layout.symbol_token_kinds = {0};
    auto valid = encode(layout);
    ZASSERT(valid);
    ZASSERT(loaded.load_table(bytes_of(*valid), pins));
}

ZEST_CASE(TableReservedKeysRejected) {
    // The two DenseMap sentinel key values can never sit in the in-memory
    // tables, so the writer can never emit them; a blob carrying one is
    // corrupt, and inserting it would corrupt (or assert in) the loader's
    // own containers.
    clice::FileTable pool;
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    index::ProjectIndex loaded{pool};

    {
        auto layout = project_table_blob();
        layout.version_ids = {0xffffffffu};
        layout.version_paths = {"/proj/a.h"};
        layout.version_hashes = {0x1};
        auto bytes = encode(layout);
        ZASSERT(bytes);
        ZASSERT(!loaded.load_table(bytes_of(*bytes), pins).has_value());
    }
    {
        clice::Bitmap bits;
        bits.add(3);
        auto layout = project_table_blob();
        layout.paths = four_paths();
        add_symbol(layout, ~std::uint64_t(0), "sym", file_set(bits));
        auto bytes = encode(layout);
        ZASSERT(bytes);
        ZASSERT(!loaded.load_table(bytes_of(*bytes), pins).has_value());
    }
    {
        auto layout = project_table_blob();
        add_symbol(layout, 42, "sym", file_set(clice::Bitmap{}), ~std::uint64_t(0));
        auto bytes = encode(layout);
        ZASSERT(bytes);
        ZASSERT(!loaded.load_table(bytes_of(*bytes), pins).has_value());
    }
    {
        auto layout = project_table_blob();
        layout.paths = {""};
        auto bytes = encode(layout);
        ZASSERT(bytes);
        ZASSERT(!loaded.load_table(bytes_of(*bytes), pins).has_value());
    }
    ZASSERT(pool.versions.empty());
    ZASSERT(loaded.symbol_count() == 0u);
}

ZEST_CASE(UnknownFileVersionsDetected) {
    auto layout = project_table_blob();
    layout.next_version_id = 4;
    layout.version_ids = {3};
    layout.version_paths = {"/proj/a.h"};
    layout.version_hashes = {0x1};
    auto bytes = encode(layout);
    ZASSERT(bytes);

    clice::FileTable pool;
    pool.intern_version(pool.intern(Spelling::absolute("/proj/opened-first.cpp")), 0x9);
    index::ProjectIndex loaded{pool};
    llvm::DenseMap<VersionID, std::uint64_t> pins;
    ZASSERT(loaded.load_table(bytes_of(*bytes), pins));
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
    std::string first_bytes;
    llvm::raw_string_ostream first_os(first_bytes);
    first.serialize_table(first_os);

    clice::FileTable second_pool;
    second_pool.intern_version(second_pool.intern(Spelling::absolute("/app/pad.cpp")), 0x5);
    auto second = build_project(second_pool, "/lib/used.h", "/app/tu.cpp");
    auto* written = second.held_manifest(second_pool.intern(Spelling::absolute("/app/tu.cpp")));
    ZASSERT(written);
    auto persisted = second.export_manifest(*written);
    std::string second_bytes;
    llvm::raw_string_ostream second_os(second_bytes);
    second.serialize_table(second_os);

    clice::FileTable shared;
    index::ProjectIndex first_loaded{shared};
    index::ProjectIndex second_loaded{shared};
    llvm::DenseMap<VersionID, std::uint64_t> first_pins;
    llvm::DenseMap<VersionID, std::uint64_t> second_pins;
    ZASSERT(first_loaded.load_table(first_bytes, first_pins));
    ZASSERT(second_loaded.load_table(second_bytes, second_pins));

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
    ZASSERT(second_pins.size() == std::size_t(1));
    ZASSERT(second_pins.contains(imported.tu_version));
    ZASSERT(second_loaded.export_manifest(imported) == persisted);
}

};  // ZEST_SUITE(ProjectTable)

}  // namespace
}  // namespace clice::testing
