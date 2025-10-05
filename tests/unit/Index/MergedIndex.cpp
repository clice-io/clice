#include "Test/Tester.h"
#include "Index/MergedIndex.h"

namespace clice::testing {

namespace {

suite<"MergedIndex"> suite = [] {
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

    test("Assert") = [&] {
        build_index(R"(
            #include <iostream>
        )");

        std::println("{}", tu_index.file_indices.size());

        index::MergedIndex merged;

        for(auto& [fid, index]: tu_index.file_indices) {
            auto path = tester.unit->file_path(fid);

            if(path.ends_with("stddef.h")) {
                merged.merge(path, tu_index.graph.getInclude(fid), index);
            }
        }
    };
};

}  // namespace

}  // namespace clice::testing
