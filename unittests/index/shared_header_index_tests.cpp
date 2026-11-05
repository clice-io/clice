module clice;

import :compile.compilation;
import :compile.shared_header;
import :index.indexer;
import :index.shared_header;
import :index.symbol;
import :index.tu_index;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

/// A shared header's build of `shared.hxx` in `tmp`: its PCH at `pch`, its
/// record with its sections.
std::optional<index::SharedHeaderIndex> build_shared(TempDir& tmp, llvm::StringRef pch) {
    CompilationParams params{
        .kind = CompilationKind::PrecompiledHeader,
        .output_file = pch.str(),
        .header_identities = true,
        .record_shared = true,
    };
    params.input.directory = std::string(tmp.root);
    params.input.arguments = {"clang++",
                              "-x",
                              "c++-header",
                              "-std=c++20",
                              "-resource-dir",
                              std::string(resource_dir()),
                              "-c",
                              tmp.path("shared.hxx")};
    auto unit = compile(std::move(params));
    if(!unit->has_output() || !collect_errors(*unit).empty()) {
        return std::nullopt;
    }
    auto tu_index = index::build_tu_index(*unit);
    auto index = index::shared_header_index(unit->shared_record(), as_chars(tu_index));
    // As the cache stores it.
    return index::decode_shared_header(as_chars(index::encode_shared_header(index)));
}

/// The TU index of `main.cpp` in `tmp` loading the PCH at `pch`.
std::string index_over(TempDir& tmp, llvm::StringRef pch, const index::SharedHeaderIndex& shared) {
    CompilationParams params{
        .kind = CompilationKind::Whole,
        .header_identities = true,
        .shared = std::shared_ptr<const SharedHeaderRecord>(std::shared_ptr<void>(), &shared.record),
    };
    params.input.directory = std::string(tmp.root);
    params.input.arguments = {"clang++",
                              "-fsyntax-only",
                              "-std=c++20",
                              "-resource-dir",
                              std::string(resource_dir()),
                              "-c",
                              tmp.path("main.cpp")};
    params.input.pch = {pch.str(), 0};
    auto unit = compile(std::move(params));
    ZEXPECT(unit->shared_rejection().empty());
    return std::string(
        as_chars(index::build_tu_index(*unit, {.shared_sections = shared.sections})));
}

/// The include tree as `includer line -> file`, a skipped include marked.
std::vector<std::string> includes(const index::TUIndex& view) {
    std::vector<std::string> lines;
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        auto node = view.node(i);
        auto parent = node.parent == index::no_node
                          ? std::string("main.cpp")
                          : std::string(path::filename(view.path(view.node(node.parent).file)));
        lines.push_back(std::format("{}:{} -> {}{}",
                                    parent,
                                    node.line,
                                    path::filename(view.path(node.file)),
                                    node.skipped ? " skipped" : ""));
    }
    ranges::sort(lines);
    return lines;
}

/// Each section as `file hash`.
std::vector<std::string> sections(const index::TUIndex& view) {
    std::vector<std::string> lines;
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        lines.push_back(std::format("{} {:016x}",
                                    path::filename(view.path(view.section_path(i))),
                                    view.section_hash(i)));
    }
    ranges::sort(lines);
    return lines;
}

ZEST_SUITE(SharedHeaderIndex) {

ZEST_CASE(SameAsParsedByTheUnit) {
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"outer.h\"\n#include \"other.h\"\n");
    tmp.touch("inner.h", "#pragma once\nstruct Inner { int value; };\n");
    tmp.touch("outer.h", R"cpp(#pragma once
#include "inner.h"
struct Outer { Inner inner; };
inline int outer(const Outer& o) { return o.inner.value; }
)cpp");
    tmp.touch("other.h", "#pragma once\n#include \"inner.h\"\nint other(Inner);\n");
    tmp.touch("main.cpp", R"cpp(#include "other.h"
#include "outer.h"
int use() { return outer(Outer{}) + other(Inner{}); }
)cpp");
    auto pch = tmp.path("shared.pch");
    auto shared = build_shared(tmp, pch);
    ZASSERT(shared);

    auto walked_data = index_file(tmp, tmp.path("main.cpp"), {"-std=c++20"}).data;
    auto grafted_data = index_over(tmp, pch, *shared);
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto grafted = index::TUIndex::from_bytes(grafted_data);
    ZASSERT(walked.loaded());
    ZASSERT(grafted.loaded());
    ZEXPECT(includes(grafted) == includes(walked));
    ZEXPECT(sections(grafted) == sections(walked));
    // Its headers' variants travel as bare hashes.
    for(std::uint32_t i = 0; i < grafted.section_count(); i += 1) {
        if(grafted.section_path(i) != grafted.focus_file()) {
            ZEXPECT(grafted.section_blob(i).empty());
        }
    }
};

};  // ZEST_SUITE(SharedHeaderIndex)

}  // namespace

}  // namespace clice::testing
