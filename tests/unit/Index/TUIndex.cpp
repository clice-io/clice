#include "Index/TUIndex.h"
#include "Test/Tester.h"

namespace clice::testing {

namespace {

suite<"TUIndex"> suite = [] {
    Tester tester;
    index::TUIndex tu_index;

    auto build_index = [&](llvm::StringRef code,
                           std::source_location location = std::source_location::current()) {
        tester.clear();
        tester.add_main("main.cpp", code);
        fatal / expect(tester.compile(), location);

        tu_index = index::TUIndex::build(*tester.unit);
    };

    auto expect_select = [&](llvm::StringRef pos,
                             llvm::StringRef expect_range,
                             llvm::StringRef file = "",
                             std::source_location location = std::source_location::current()) {
        auto offset = tester.point(pos, file);
        auto range = tester.range(expect_range, file);

        auto fid = file.empty() ? tester.unit->interested_file() : tester.unit->file_id(file);
        auto& index = tu_index.file_indices[fid];

        auto it = std::ranges::lower_bound(
            index.occurrences,
            offset,
            {},
            [](index::Occurrence& occurrence) { return occurrence.range.end; });

        auto err =
            std::format("Fail to find symbol for offser: {} range: range: {}", offset, dump(range));

        fatal / expect(it != index.occurrences.end(), location) << err;

        /// FIXME: Make eq pretty print reflectable struct.
        expect(eq(dump(it->range), dump(range)), location);
    };

    test("Basic") = [&] {
        build_index(R"(
            int @1[f$(1)oo]();

            int @2[b$(2)ar]() {
                return @3[fo$(3)o]() + 1;
            }
        )");

        expect(eq(tu_index.file_indices.size(), 1));
        auto& index = tu_index.file_indices.begin()->second;
        expect(eq(index.relations.size(), 2));
        expect(eq(index.occurrences.size(), 3));

        expect_select("1", "1");
        expect_select("2", "2");
        expect_select("3", "3");
    };
};

}  // namespace

}  // namespace clice::testing
