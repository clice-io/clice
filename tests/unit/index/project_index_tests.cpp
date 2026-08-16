#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/project_index.h"
#include "index/serialization.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

TEST_SUITE(ProjectIndex, Tester) {

std::string wire;

/// Build the current unit's TUIndex and return the zero-copy view the
/// merge path consumes; `wire` keeps the bytes alive.
std::optional<index::TUIndexView> build_view() {
    auto tu_index = index::TUIndex::build(*unit);
    wire.clear();
    llvm::raw_string_ostream os(wire);
    tu_index.serialize(os);
    return index::TUIndexView::from(wire);
}

index::SymbolHash find_symbol(const index::ProjectIndex& project, llvm::StringRef name) {
    for(auto& [hash, symbol]: project.symbols) {
        if(symbol.name == name) {
            return hash;
        }
    }
    return 0;
}

/// The TU-local id -> pool id mapping merge() consumes, as Indexer::merge
/// computes it.
llvm::SmallVector<std::uint32_t> intern_paths(const index::TUIndexView& view,
                                              clice::PathPool& pool) {
    llvm::SmallVector<std::uint32_t> ids;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        ids.push_back(pool.intern(view.path(i)));
    }
    return ids;
}

llvm::StringRef bytes_of(const std::vector<std::uint8_t>& blob) {
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

TEST_CASE(MergeCollectsExternalSymbols) {
    add_file("header.h", R"(
        int external_fn();
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        static int local_fn() { return 1; }
        int use() { return external_fn() + local_fn(); }
    )");
    ASSERT_TRUE(compile());

    clice::PathPool pool;
    index::ProjectIndex project;
    auto view = build_view();
    ASSERT_TRUE(view.has_value());
    ASSERT_TRUE(project.merge(*view, intern_paths(*view, pool)));

    auto external = find_symbol(project, "external_fn");
    ASSERT_TRUE(external != 0);
    // Referenced from both the header (declaration) and the main file.
    ASSERT_TRUE(project.symbols[external].reference_files.cardinality() >= 2);

    // Non-External symbols never reach the project table.
    ASSERT_EQ(find_symbol(project, "local_fn"), 0u);
}

TEST_CASE(MergeRejectsBadBitmap) {
    // Field order MUST mirror TUIndex up to `symbols` (the skip-annotated
    // file_indices holds no slot): serialize() always writes valid bitmap
    // images, so a malformed one has to be planted by hand.
    struct SymbolMirror {
        std::string name;
        std::uint8_t kind = 0;
        std::uint8_t scope = 0;
        std::vector<std::byte> reference_files;
    };

    struct TUIndexPrefixMirror {
        std::uint32_t format_version = 0;
        std::int64_t built_at = 0;
        index::IncludeGraph graph;
        llvm::DenseMap<std::uint64_t, SymbolMirror> symbols{};
    };

    TUIndexPrefixMirror mirror;
    mirror.format_version = index::index_format_version;
    mirror.graph.paths = {"/proj/main.cpp"};
    clice::Bitmap bits;
    bits.add(0);
    mirror.symbols[42] = {.name = "good_sym", .reference_files = index::write_bitmap(bits)};

    // Control: the mirror layout matches — the view sees the symbol and a
    // valid image merges.
    auto valid = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(valid.has_value());
    auto valid_view = index::TUIndexView::from(bytes_of(*valid));
    ASSERT_TRUE(valid_view.has_value());
    clice::PathPool pool;
    index::ProjectIndex accepting;
    ASSERT_TRUE(accepting.merge(*valid_view, intern_paths(*valid_view, pool)));
    ASSERT_EQ(find_symbol(accepting, "good_sym"), 42u);

    // One malformed image rejects the whole result: merged bits would
    // persist behind versions that match the disk, with the lost ones
    // never rebuilt. The symbols that decoded fine must not stay behind.
    mirror.symbols[43] = {
        .name = "bad_sym",
        .reference_files = {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}},
    };
    auto corrupt = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(corrupt.has_value());
    auto corrupt_view = index::TUIndexView::from(bytes_of(*corrupt));
    ASSERT_TRUE(corrupt_view.has_value());
    index::ProjectIndex rejecting;
    ASSERT_FALSE(rejecting.merge(*corrupt_view, intern_paths(*corrupt_view, pool)));
    ASSERT_TRUE(rejecting.symbols.empty());

    // An id past the path table is the same corruption in a decodable
    // coat: silently dropped, the symbol's relations would sit in a shard
    // its fan-out never visits — reject like the full TUIndex::from does.
    clice::Bitmap stray;
    stray.add(7);
    mirror.symbols[43] = {.name = "bad_sym", .reference_files = index::write_bitmap(stray)};
    auto out_of_range = kota::codec::fbs::to_bytes(mirror);
    ASSERT_TRUE(out_of_range.has_value());
    auto stray_view = index::TUIndexView::from(bytes_of(*out_of_range));
    ASSERT_TRUE(stray_view.has_value());
    index::ProjectIndex bounding;
    ASSERT_FALSE(bounding.merge(*stray_view, intern_paths(*stray_view, pool)));
    ASSERT_TRUE(bounding.symbols.empty());
}

TEST_CASE(FileVersionInterning) {
    index::ProjectIndex project;
    auto a = project.intern_file_version(7, 0x1111);
    ASSERT_EQ(project.intern_file_version(7, 0x1111), a);

    auto b = project.intern_file_version(7, 0x2222);
    ASSERT_TRUE(b != a);
    ASSERT_EQ(project.file_versions.find(b)->second.path_id, 7u);
    ASSERT_EQ(project.file_versions.find(b)->second.content_hash, 0x2222u);
}

TEST_CASE(ManifestContributions) {
    index::ProjectIndex project;
    auto fv_a = project.intern_file_version(1, 0xa);
    auto fv_b = project.intern_file_version(2, 0xb);

    auto manifest_for = [&](std::uint32_t tu_fv,
                            std::initializer_list<std::pair<std::uint32_t, std::uint64_t>> rows) {
        index::TUManifest manifest;
        manifest.tu_fv = tu_fv;
        manifest.contributions = rows;
        return manifest;
    };

    auto tu1_fv = project.intern_file_version(10, 0x1);
    auto tu2_fv = project.intern_file_version(11, 0x2);

    // TU 1 contributes h1 to file 1 and h2 to file 2.
    auto affected = project.apply_manifest(10,
                                           manifest_for(tu1_fv,
                                                        {
                                                            {fv_a, 100},
                                                            {fv_b, 200}
    }));
    ASSERT_EQ(affected.size(), std::size_t(2));
    ASSERT_EQ(project.live_variants(1).size(), std::size_t(1));

    // TU 2 shares file 1's variant: the live set does not grow.
    project.apply_manifest(11,
                           manifest_for(tu2_fv,
                                        {
                                            {fv_a, 100}
    }));
    ASSERT_EQ(project.live_variants(1).size(), std::size_t(1));

    // TU 1 re-indexes with a new variant for file 1 and drops file 2: both
    // hashes stay live on file 1 (TU 2 still holds the old one), file 2
    // loses its only contribution.
    project.apply_manifest(10,
                           manifest_for(tu1_fv,
                                        {
                                            {fv_a, 300}
    }));
    ASSERT_EQ(project.live_variants(1).size(), std::size_t(2));
    ASSERT_TRUE(project.live_variants(2).empty());

    project.remove_manifest(11);
    auto live = project.live_variants(1);
    ASSERT_EQ(live.size(), std::size_t(1));
    ASSERT_EQ(live.front(), 300u);

    project.remove_manifest(10);
    ASSERT_TRUE(project.contributions.empty());
}

TEST_CASE(GlobalRoundTripWithRealMerge) {
    add_main("main.cpp", R"(
        int global_value = 42;
        int reader() { return global_value; }
    )");
    ASSERT_TRUE(compile());

    clice::PathPool pool;
    index::ProjectIndex project;
    auto view = build_view();
    ASSERT_TRUE(view.has_value());
    auto file_ids_map = intern_paths(*view, pool);
    ASSERT_TRUE(project.merge(*view, file_ids_map));

    // A manifest referencing the main file keeps its FileVersion alive
    // through the write's garbage collection.
    auto main_fv = project.intern_file_version(file_ids_map[view->path_count() - 1],
                                               view->path_hash(view->path_count() - 1));
    index::TUManifest manifest;
    manifest.tu_fv = main_fv;
    project.apply_manifest(file_ids_map[view->path_count() - 1], std::move(manifest));

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize_global(os, pool);

    clice::PathPool fresh;
    index::ProjectIndex loaded;
    llvm::DenseMap<std::uint32_t, std::uint64_t> pins;
    ASSERT_TRUE(loaded.load_global(buf.str(), fresh, pins));

    auto symbol = find_symbol(loaded, "global_value");
    ASSERT_TRUE(symbol != 0);
    auto main_path = pool.resolve(file_ids_map[view->path_count() - 1]);
    auto fresh_id = fresh.find(main_path);
    ASSERT_TRUE(fresh_id.has_value());
    ASSERT_TRUE(loaded.symbols[symbol].reference_files.contains(*fresh_id));
}

};  // TEST_SUITE(ProjectIndex)

}  // namespace
}  // namespace clice::testing
