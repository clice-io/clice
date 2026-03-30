#include "test/test.h"
#include "test/tester.h"
#include "index/merged_index.h"

namespace clice::testing {

namespace {

TEST_SUITE(MergedIndex, Tester) {

index::TUIndex tu_index;

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    tu_index = index::TUIndex::build(*unit);
};

void EXPECT_SELECT(llvm::StringRef pos,
                   llvm::StringRef expect_range,
                   llvm::StringRef file = "",
                   std::source_location location = std::source_location::current()) {
    auto offset = point(pos, file);
    auto expected = range(expect_range, file);

    auto fid = file.empty() ? unit->interested_file() : unit->file_id(file);
    auto& index = tu_index.file_indices[fid];

    auto it =
        std::ranges::lower_bound(index.occurrences, offset, {}, [](index::Occurrence& occurrence) {
            return occurrence.range.end;
        });

    auto err =
        std::format("Fail to find symbol for offser: {} range: range: {}", offset, dump(expected));

    ASSERT_TRUE(it != index.occurrences.end());

    /// FIXME: Make eq pretty print reflectable struct.
    ASSERT_EQ(dump(it->range), dump(expected));
}

TEST_CASE(Serialization) {
    build_index(R"(
            struct Foo { int x; int y; };
            Foo make_foo() { return Foo{1, 2}; }
            int use_foo() { return make_foo().x; }
        )");

    llvm::StringMap<index::MergedIndex> merged_indices;
    auto& graph = tu_index.graph;
    for(auto& [fid, index]: tu_index.file_indices) {
        llvm::StringRef path = graph.paths[graph.path_id(fid)];
        merged_indices[path].merge(0, graph.include_location_id(fid), index);
    }

    for(auto& [path, merged]: merged_indices) {
        llvm::SmallString<1024> s;
        llvm::raw_svector_ostream os(s);

        merged.serialize(os);

        auto view = index::MergedIndex(s);
        ASSERT_TRUE(merged == view);
    }
}

TEST_CASE(LookupByOffset) {
    build_index(R"(
            int @func[$(func)foo]() { return 42; }
            int bar() { return @ref[$(ref)foo](); }
        )");

    // Merge the main file index into a MergedIndex.
    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge(0, tu_index.graph.include_location_id(fid), tu_index.main_file_index);

    // Lookup at the reference offset should find an occurrence.
    auto ref_offset = point("ref");
    bool found = false;
    merged.lookup(ref_offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(ref_offset)) {
            found = true;
        }
        return true;
    });
    ASSERT_TRUE(found);
}

TEST_CASE(LookupBySymbolAndKind) {
    build_index(R"(
            void $(target)target_func() {}
            void caller() { $(call)target_func(); }
        )");

    index::MergedIndex merged;
    auto fid = unit->interested_file();
    merged.merge(0, tu_index.graph.include_location_id(fid), tu_index.main_file_index);

    // Find the target_func symbol hash via occurrence lookup.
    auto target_offset = point("target");
    index::SymbolHash target_hash = 0;
    merged.lookup(target_offset, [&](const index::Occurrence& occ) {
        if(occ.range.contains(target_offset)) {
            target_hash = occ.target;
            return false;
        }
        return true;
    });
    ASSERT_TRUE(target_hash != 0);

    // Lookup Definition relation for the symbol.
    bool found_def = false;
    merged.lookup(target_hash, RelationKind::Definition, [&](const index::Relation& rel) {
        found_def = true;
        return true;
    });
    ASSERT_TRUE(found_def);
}

TEST_CASE(MultipleMergesDedup) {
    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("a.cpp", R"(
            #include "header.h"
            int use_a() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    auto tu_a = index::TUIndex::build(*unit);

    add_file("header.h", R"(
            #pragma once
            inline int shared() { return 1; }
        )");
    add_main("b.cpp", R"(
            #include "header.h"
            int use_b() { return shared(); }
        )");
    ASSERT_TRUE(compile());
    auto tu_b = index::TUIndex::build(*unit);

    // Merge header indices from both TUs into same MergedIndex.
    index::MergedIndex merged_header;
    for(auto& [fid, file_index]: tu_a.file_indices) {
        merged_header.merge(0, tu_a.graph.include_location_id(fid), file_index);
    }
    for(auto& [fid, file_index]: tu_b.file_indices) {
        merged_header.merge(1, tu_b.graph.include_location_id(fid), file_index);
    }

    // Serialize and deserialize to verify dedup survives round-trip.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged_header.serialize(os);

    auto restored = index::MergedIndex(buf);
    ASSERT_TRUE(merged_header == restored);
}

TEST_CASE(SerializationRoundTripInMemory) {
    build_index(R"(
            struct Foo { int x; };
            Foo make() { return Foo{42}; }
        )");

    // Merge using the include_id overload (same as existing Serialization test).
    index::MergedIndex merged;
    auto fid = unit->interested_file();
    auto include_id = tu_index.graph.include_location_id(fid);
    merged.merge(0, include_id, tu_index.main_file_index);

    // Serialize.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    merged.serialize(os);

    // Deserialize and compare.
    auto restored = index::MergedIndex(buf);
    ASSERT_TRUE(merged == restored);

    // Lookup should work on the deserialized version too.
    bool found = false;
    for(auto& occ: tu_index.main_file_index.occurrences) {
        restored.lookup(occ.range.begin, [&](const index::Occurrence& o) {
            if(o.range.begin == occ.range.begin) {
                found = true;
            }
            return true;
        });
        if(found)
            break;
    }
    ASSERT_TRUE(found);
}

};  // TEST_SUITE(MergedIndex)
}  // namespace
}  // namespace clice::testing
