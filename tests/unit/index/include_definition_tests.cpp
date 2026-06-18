#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/merged_index.h"
#include "index/tu_index.h"
#include "support/filesystem.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

namespace {

TEST_SUITE(include_definition, Tester) {

auto target_name(const index::TUIndex& idx, std::optional<std::uint32_t> target) -> std::string {
    if(!target || *target >= idx.graph.paths.size()) {
        return {};
    }
    return path::filename(idx.graph.paths[*target]).str();
}

auto format_point(llvm::StringRef name,
                  const index::TUIndex& idx,
                  std::optional<std::uint32_t> target) -> std::string {
    auto result = std::format("- {{ point: {}", yaml_str(name));
    auto file = target_name(idx, target);
    if(!file.empty()) {
        result += std::format(", target: {}, range: \"0:0-0:0\"", yaml_str(file));
    }
    result += " }";
    return result;
}

TEST_CASE(snapshot) {
    ASSERT_SNAPSHOT_GLOB(corpus_dir, "definition/**/*.test", ([&](std::string_view path) {
        auto content = fs::read(path);
        if(!content) {
            return std::string("READ_ERROR");
        }

        clear();
        add_files("main.cpp", *content);
        if(!compile()) {
            return std::string("COMPILE_ERROR");
        }

        auto idx = index::TUIndex::build(*unit, true);
        auto locations = idx.graph.locations;

        index::MergedIndex merged;
        merged.merge(0,
                     idx.built_at,
                     std::move(locations),
                     idx.main_file_index,
                     sources.all_files["main.cpp"].content);

        llvm::SmallString<4096> buffer;
        llvm::raw_svector_ostream os(buffer);
        merged.serialize(os);
        auto restored = index::MergedIndex(buffer);

        std::vector<std::pair<std::string, std::uint32_t>> points;
        for(auto& [name, offset]: sources.all_files["main.cpp"].offsets) {
            points.emplace_back(name.str(), offset);
        }
        std::ranges::sort(points);

        std::string result;
        for(auto& [name, offset]: points) {
            if(!result.empty()) {
                result += '\n';
            }
            result += format_point(name, idx, restored.find_include_definition(offset));
        }
        return result;
    }));
}

};  // TEST_SUITE(include_definition)

}  // namespace

}  // namespace clice::testing
