module clice;

import :compile.compilation;
import :compile.shared_header;
import :index.indexer;
import :index.instantiation_shard;
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
/// record with its sections, and its own TU index into `tu_index`.
std::optional<index::SharedHeaderIndex> build_shared(TempDir& tmp,
                                                     llvm::StringRef pch,
                                                     std::string* tu_index = nullptr) {
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
    auto bytes = index::build_tu_index(*unit);
    if(tu_index) {
        *tu_index = as_chars(bytes).str();
    }
    auto index = index::shared_header_index(unit->shared_record(), as_chars(bytes));
    // As the cache stores it.
    return index::decode_shared_header(as_chars(index::encode_shared_header(index)));
}

/// The TU index of `main.cpp` in `tmp` loading the PCH at `pch`, with its
/// record as a shared header's when given one.
std::string index_over(TempDir& tmp,
                       llvm::StringRef pch,
                       const index::SharedHeaderIndex* shared = nullptr) {
    CompilationParams params{.kind = CompilationKind::Whole, .header_identities = true};
    if(shared) {
        params.shared =
            std::shared_ptr<const SharedHeaderRecord>(std::shared_ptr<void>(), &shared->record);
    }
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
    index::TUIndexOptions options;
    if(shared) {
        options.shared_sections = shared->sections;
        options.shared_resolutions = shared->resolutions;
    }
    return std::string(as_chars(index::build_tu_index(*unit, options)));
}

/// The flags of the symbol named `name` in the TU index; none for none.
std::optional<index::SymbolFlags> flags_of(const index::TUIndex& view, llvm::StringRef name) {
    std::optional<index::SymbolFlags> flags;
    view.for_each_symbol(
        [&](index::SymbolHash, const index::SymbolView& symbol, llvm::ArrayRef<std::uint32_t>) {
            if(symbol.name == name) {
                flags = symbol.flags;
            }
            return true;
        });
    return flags;
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

/// The resolutions each file's instantiation section uses, made or used as
/// stored, as `file resolution`.
std::vector<std::string> resolutions(const index::TUIndex& view) {
    std::vector<std::string> lines;
    for(std::uint32_t i = 0; i < view.instantiation_count(); i += 1) {
        auto file = path::filename(view.path(view.instantiation_path(i)));
        std::vector<std::uint64_t> used(view.used_resolutions(i).begin(),
                                        view.used_resolutions(i).end());
        if(auto blob = view.instantiation_blob(i); !blob.empty()) {
            used.append_range(index::InstantiationShard::from_bytes(blob).resolutions());
        }
        for(auto resolution: used) {
            lines.push_back(std::format("{} {:016x}", file, resolution));
        }
    }
    ranges::sort(lines);
    lines.erase(ranges::unique(lines).begin(), lines.end());
    return lines;
}

/// The instantiation records of `file` in the TU index, as text.
std::set<std::string> records(const index::TUIndex& view, llvm::StringRef file) {
    std::set<std::string> lines;
    for(std::uint32_t i = 0; i < view.instantiation_count(); i += 1) {
        auto blob = view.instantiation_blob(i);
        if(llvm::StringRef(path::filename(view.path(view.instantiation_path(i)))) != file ||
           blob.empty()) {
            continue;
        }
        index::InstantiationShard::from_bytes(blob).for_each_record(
            [&](const index::InstantiationRecord& record) {
                lines.insert(std::format("{}-{} {} {:016x} {:016x}",
                                         record.range.begin,
                                         record.range.end,
                                         static_cast<int>(record.kind),
                                         record.target,
                                         record.from));
                return true;
            });
    }
    return lines;
}

ZEST_SUITE(SharedHeaderIndex) {

ZEST_CASE(SameAsParsedByTheUnit) {
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"outer.h\"\n#include \"other.h\"\n");
    tmp.touch("inner.h", "#pragma once\nstruct Inner { int value; };\n");
    tmp.touch("outer.h", R"cpp(#pragma once
#include "inner.h"
template <typename T>
struct Box {
    T value;
    auto get() const { return value.value; }
};
struct Outer { Box<Inner> inner; };
inline int outer(const Outer& o) { return o.inner.get(); }
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
    auto grafted_data = index_over(tmp, pch, &*shared);
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto grafted = index::TUIndex::from_bytes(grafted_data);
    ZASSERT(walked.loaded());
    ZASSERT(grafted.loaded());
    ZEXPECT(includes(grafted) == includes(walked));
    ZEXPECT(sections(grafted) == sections(walked));
    ZEXPECT(!resolutions(walked).empty());
    ZEXPECT(resolutions(grafted) == resolutions(walked));
    // Its headers' variants travel as bare hashes.
    for(std::uint32_t i = 0; i < grafted.section_count(); i += 1) {
        if(grafted.section_path(i) != grafted.focus_file()) {
            ZEXPECT(grafted.section_blob(i).empty());
        }
    }
};

ZEST_CASE(LocalOfThePCHDefined) {
    // A constant of a header the PCH holds has internal linkage: the unit's
    // own table describes it, defined, whether it parses the header or
    // reads it from a precompiled header, shared or the build's.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"limits.h\"\n");
    tmp.touch("limits.h", "#pragma once\nconstexpr int limit = 3;\n");
    tmp.touch("main.cpp", "#include \"limits.h\"\nint use() { return limit; }\n");
    auto pch = tmp.path("shared.pch");
    auto shared = build_shared(tmp, pch);
    ZASSERT(shared);

    auto walked_data = index_file(tmp, tmp.path("main.cpp"), {"-std=c++20"}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto walked_flags = flags_of(walked, "limit");
    ZASSERT(walked_flags);
    ZEXPECT(has_flag(*walked_flags, index::SymbolFlags::HasDefinition));

    auto grafted_data = index_over(tmp, pch, &*shared);
    auto grafted = index::TUIndex::from_bytes(grafted_data);
    ZEXPECT(flags_of(grafted, "limit") == walked_flags);

    auto declared_data = index_over(tmp, pch);
    auto declared = index::TUIndex::from_bytes(declared_data);
    auto declared_flags = flags_of(declared, "limit");
    ZASSERT(declared_flags);
    ZEXPECT(has_flag(*declared_flags, index::SymbolFlags::HasDefinition));
};

ZEST_CASE(OwnInstantiationsOfHeldTemplates) {
    // The unit instantiates a template the PCH holds for a type of its own:
    // what it records there is what the template's text leaves open, as
    // when it parses the header itself.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"box.h\"\n");
    tmp.touch("box.h", R"cpp(#pragma once
template <typename T>
struct Box {
    T value;
    int count() const { return value.size() + 1; }
};
struct Inner {
    int size() const { return 0; }
};
inline int inner() { return Box<Inner>{}.count(); }
)cpp");
    tmp.touch("main.cpp", R"cpp(#include "box.h"
struct Mine {
    int size() const { return 1; }
};
int use() { return Box<Mine>{}.count(); }
)cpp");
    auto pch = tmp.path("shared.pch");
    std::string own_data;
    auto shared = build_shared(tmp, pch, &own_data);
    ZASSERT(shared);
    auto own = index::TUIndex::from_bytes(own_data);

    auto walked_data = index_file(tmp, tmp.path("main.cpp"), {"-std=c++20"}).data;
    auto walked = records(index::TUIndex::from_bytes(walked_data), "box.h");
    ZEXPECT(!walked.empty());
    for(auto* record: {static_cast<const index::SharedHeaderIndex*>(&*shared),
                       static_cast<const index::SharedHeaderIndex*>(nullptr)}) {
        auto data = index_over(tmp, pch, record);
        auto over = records(index::TUIndex::from_bytes(data), "box.h");
        over.insert_range(records(own, "box.h"));
        ZEXPECT(over == walked);
    }
};

ZEST_CASE(PCHResolutionsOutliveIt) {
    // The shared header's index resolves what box.h's inline code
    // instantiates; the unit uses it as stored, and keeps it once the
    // shared header's index is dropped after its pass.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"box.h\"\n");
    tmp.touch("box.h", R"cpp(#pragma once
template <typename T>
struct Box {
    T value;
    void show() { value.print(); }
};
struct Inner {
    void print() {}
};
#define SHOW(box) (box).show()
inline void inner() { SHOW(Box<Inner>{}); }
)cpp");
    tmp.touch("main.cpp", "#include \"box.h\"\nvoid use() { inner(); }\n");
    auto pch = tmp.path("shared.pch");
    std::string own;
    auto shared = build_shared(tmp, pch, &own);
    ZASSERT(shared);

    IndexerFixture f;
    open_store(tmp, f.project);
    ZASSERT(f.merge(own));
    ZASSERT(f.merge(index_over(tmp, pch, &*shared)));
    auto box = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
    auto records = [&] {
        std::size_t count = 0;
        if(auto* held = f.project.index.instantiation_shard(box)) {
            held->for_each_record([&](const index::InstantiationRecord&) {
                count += 1;
                return true;
            });
        }
        return count;
    };
    auto before = records();
    ZEXPECT(before != 0U);
    f.drop_index(f.project.file_table.intern(Spelling::absolute(tmp.path("shared.hxx"))));
    f.save();
    ZEXPECT(records() == before);
};

};  // ZEST_SUITE(SharedHeaderIndex)

}  // namespace

}  // namespace clice::testing
