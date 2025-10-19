#include "Test/Tester.h"
#include "Index/MergedIndex.h"
#include "Async/Async.h"

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

    test("Serialization") = [&] {
        build_index(R"(
            #include <iostream>

            int main () {
                std::cout << "Hello world!" << std::endl;
                return 0;
            }
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
            expect(merged == view);
        }
    };
};

}  // namespace

}  // namespace clice::testing
