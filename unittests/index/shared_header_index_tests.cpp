module clice;

import :compile.compilation;
import :compile.shared_header;
import :index.indexer;
import :index.instantiation_shard;
import :index.shard;
import :index.shared_header;
import :index.symbol;
import :index.tu_index;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {

namespace {

/// A shared header's build of `shared.hxx` in `tmp`: its PCH at `pch`, its
/// record with its sections, and its own TU index into `tu_index`.
std::optional<index::SharedHeaderIndex> build_shared(TempDir& tmp,
                                                     llvm::StringRef pch,
                                                     std::string* tu_index = nullptr,
                                                     llvm::ArrayRef<std::string> extra = {}) {
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
    params.input.arguments.insert(params.input.arguments.end() - 1, extra.begin(), extra.end());
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
                       const index::SharedHeaderIndex* shared = nullptr,
                       llvm::ArrayRef<std::string> extra = {}) {
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
    params.input.arguments.insert(params.input.arguments.end() - 1, extra.begin(), extra.end());
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

/// The occurrences of `shard` as `begin-end symbol`.
std::set<std::string> occurrences(const index::Shard& shard) {
    std::set<std::string> lines;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        lines.insert(std::format("{}-{} {:016x}",
                                 occurrence.range.begin,
                                 occurrence.range.end,
                                 occurrence.target));
        return true;
    });
    return lines;
}

ZEST_SUITE(SharedHeaderIndex) {

ZEST_CASE(SameAsUnitParse) {
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

ZEST_CASE(PCHLocalDefined) {
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

ZEST_CASE(HeldTemplateInstantiations) {
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
    // Before the shared header's index, what the unit names by hash is
    // stored nowhere.
    auto over = index_over(tmp, pch, &*shared);
    auto early = f.project.index.merge(over, {});
    ZASSERT(!early);
    ZEXPECT(early.error() == index::MergeError::Outdated);
    ZASSERT(f.merge(own));
    ZASSERT(f.merge(over));
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

ZEST_CASE(FileEnteredByBoth) {
    // kinds.def has no guard: the enumeration of the header the PCH holds
    // enters it, and the unit enters it again under a macro of its own. The
    // unit's entries of it are both, the shared header's index dropped too.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"kinds.h\"\n");
    tmp.touch("kinds.h", R"cpp(#pragma once
enum Kind {
#define KIND(name) name,
#include "kinds.def"
#undef KIND
};
)cpp");
    tmp.touch("kinds.def", "KIND(Alpha)\nKIND(Beta)\n");
    tmp.touch("main.cpp", R"cpp(#include "kinds.h"
#define KIND(name) int name##_value();
#include "kinds.def"
#undef KIND
int use() { return Alpha_value() + Beta; }
)cpp");
    auto pch = tmp.path("shared.pch");
    std::string own_data;
    auto shared = build_shared(tmp, pch, &own_data);
    ZASSERT(shared);
    auto own = index::TUIndex::from_bytes(own_data);
    auto over_data = index_over(tmp, pch, &*shared);
    auto over = index::TUIndex::from_bytes(over_data);
    auto walked_data = index_file(tmp, tmp.path("main.cpp"), {"-std=c++20"}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);

    auto in_own = section_of(own, "kinds.def");
    auto in_over = section_of(over, "kinds.def");
    auto in_walked = section_of(walked, "kinds.def");
    ZASSERT(in_own);
    ZASSERT(in_over);
    ZASSERT(in_walked);
    ZEXPECT(over.section_shared(*in_over) == own.section_hash(*in_own));
    auto both = occurrences(own.section_shard(*in_own));
    auto unit_own = occurrences(over.section_shard(*in_over));
    ZEXPECT(!unit_own.empty());
    both.insert_range(unit_own);
    ZEXPECT(both == occurrences(walked.section_shard(*in_walked)));

    IndexerFixture f;
    open_store(tmp, f.project);
    ZASSERT(f.merge(own_data));
    ZASSERT(f.merge(over_data));
    f.drop_index(f.project.file_table.intern(Spelling::absolute(tmp.path("shared.hxx"))));
    f.save();
    auto* stored = f.project.index.shard(
        f.project.file_table.intern(Spelling::absolute(tmp.path("kinds.def"))));
    ZASSERT(stored);
    auto variants = stored->variants();
    ZEXPECT(ranges::contains(variants, over.section_hash(*in_over)));
    ZEXPECT(ranges::contains(variants, own.section_hash(*in_own)));
};

ZEST_CASE(GuessesIntoHeldHeaders) {
    // impl.h's template finds `Traits` through user.h, which includes the
    // header the PCH holds that declares it.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"decl.h\"\n");
    tmp.touch("decl.h", "#pragma once\n#include \"helpers.h\"\n");
    tmp.touch("helpers.h", R"cpp(#pragma once
template <typename T>
struct Traits {
    using type = T;
};
)cpp");
    tmp.touch("impl.h", R"cpp(#pragma once
template <typename T>
typename Traits<T>::type convert(T value) { return value; }
)cpp");
    tmp.touch("user.h", "#pragma once\n#include \"decl.h\"\n#include \"impl.h\"\n");
    tmp.touch("main.cpp", "#include \"user.h\"\nint use() { return convert(1); }\n");
    auto pch = tmp.path("shared.pch");
    auto shared = build_shared(tmp, pch);
    ZASSERT(shared);
    auto walked_data = index_file(tmp, tmp.path("main.cpp"), {"-std=c++20"}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto over_data = index_over(tmp, pch, &*shared);
    auto over = index::TUIndex::from_bytes(over_data);
    auto in_walked = section_of(walked, "impl.h");
    auto in_over = section_of(over, "impl.h");
    ZASSERT(in_walked);
    ZASSERT(in_over);
    ZEXPECT(guessed(walked, *in_walked) == std::vector<std::string>{"type"});
    ZEXPECT(guessed(over, *in_over) == guessed(walked, *in_walked));
    ZEXPECT(over.section_identity(*in_over) == walked.section_identity(*in_walked));
};

ZEST_CASE(PCHLookupsAbsent) {
    // probe.h looks for flag.h, which is not there: creating it changes
    // what the unit compiles to.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"probe.h\"\n");
    tmp.touch("probe.h", R"cpp(#pragma once
#if __has_include("flag.h")
int flagged();
#endif
)cpp");
    tmp.touch("main.cpp", "#include \"probe.h\"\n");
    auto pch = tmp.path("shared.pch");
    auto shared = build_shared(tmp, pch);
    ZASSERT(shared);
    auto over_data = index_over(tmp, pch, &*shared);
    auto over = index::TUIndex::from_bytes(over_data);
    std::vector<std::string> absent;
    for(std::uint32_t i = 0; i < over.absent_count(); i += 1) {
        absent.emplace_back(path::filename(over.absent(i)));
    }
    ZEXPECT(ranges::contains(absent, "flag.h"));
};

ZEST_CASE(UnguardedRoot) {
    // box.h's `#pragma once` lies in a branch not taken: a unit including
    // it would parse it again over the PCH.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"box.h\"\n#include \"guarded.h\"\n");
    tmp.touch("box.h", "#if 0\n#pragma once\n#endif\nstruct Box {};\n");
    tmp.touch("guarded.h", "#pragma once\nstruct Guarded {};\n");
    auto pch = tmp.path("shared.pch");
    auto shared = build_shared(tmp, pch);
    ZASSERT(shared);
    ZEXPECT(shared->record.unheld);

    tmp.touch("shared.hxx", "#pragma once\n#include \"guarded.h\"\n");
    auto guarded = build_shared(tmp, pch);
    ZASSERT(guarded);
    ZEXPECT(!guarded->record.unheld);
};

ZEST_CASE(LocalsOutliveIt) {
    // helper has internal linkage; call.h, which the PCH holds, names it
    // as the unit does: the unit's manifest links all three files once the
    // shared header's index is dropped.
    TempDir tmp;
    tmp.touch("shared.hxx", "#pragma once\n#include \"call.h\"\n");
    tmp.touch("helper.h", "#pragma once\nstatic int helper() { return 1; }\n");
    tmp.touch("call.h",
              "#pragma once\n#include \"helper.h\"\ninline int call() { return helper(); }\n");
    tmp.touch("main.cpp", "#include \"call.h\"\nint use() { return call() + helper(); }\n");
    auto pch = tmp.path("shared.pch");
    std::string own;
    auto shared = build_shared(tmp, pch, &own);
    ZASSERT(shared);
    auto over = index_over(tmp, pch, &*shared);

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.list_units(tmp,
                     {
                         {"main.cpp", {"-std=c++20"}}
        });
        ZASSERT(f.merge(own));
        ZASSERT(f.merge(over));
        auto header = f.project.file_table.intern(Spelling::absolute(tmp.path("shared.hxx")));
        auto unit = f.project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
        auto& headers = f.project.shared_headers;
        headers.headers.push_back({.file = header, .request = {}});
        headers.files.insert(header);
        headers.units[unit] = 0;
        f.project.release_shared_headers();
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.list_units(tmp,
                 {
                     {"main.cpp", {"-std=c++20"}}
    });
    f.load();
    auto unit = f.project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    auto manifest = f.project.index.manifest(unit);
    ZASSERT(manifest);
    ZASSERT(manifest->shared_locals.size() == 1U);
    ZEXPECT(manifest->shared_locals.front().files.size() == 3U);
};

ZEST_CASE(MostUnitsReject) {
    // Once more of the units given a shared header parse without it than
    // over it, the rest are not given it.
    TempDir tmp;
    IndexerFixture f;
    auto file = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto& headers = f.project.shared_headers;
    headers.headers.push_back({.file = file("shared.hxx"), .request = {}});
    std::vector<Fid> units;
    for(int i = 0; i < 10; i += 1) {
        units.push_back(file(std::format("u{}.cpp", i)));
        headers.units[units.back()] = 0;
    }
    for(int i = 0; i < 8; i += 1) {
        f.project.shared_header_used(units[i], i % 2 == 0);
    }
    ZEXPECT(f.project.shared_header_request(units[8]).has_value());
    f.project.shared_header_used(units[8], true);
    ZEXPECT(!f.project.shared_header_request(units[9]).has_value());
};

ZEST_CASE(GuessesUnderForcedInclude) {
    // The command forces a header in, which no file of the unit includes.
    TempDir tmp;
    tmp.touch("forced.h", "#pragma once\n#define FORCED 1\n");
    tmp.touch("shared.hxx", "#pragma once\n#include \"helpers.h\"\n");
    tmp.touch("helpers.h", "#pragma once\nvoid helper(int);\n");
    tmp.touch("impl.h", R"cpp(#pragma once
#include "helpers.h"
template <typename T>
void call(T value) { helper(value); }
)cpp");
    tmp.touch("main.cpp", "#include \"impl.h\"\nvoid use() { call(FORCED); }\n");
    std::vector<std::string> forced = {"-include", tmp.path("forced.h")};
    auto pch = tmp.path("shared.pch");
    auto shared = build_shared(tmp, pch, nullptr, forced);
    ZASSERT(shared);
    auto over_data = index_over(tmp, pch, &*shared, forced);
    auto over = index::TUIndex::from_bytes(over_data);
    auto in_over = section_of(over, "impl.h");
    ZASSERT(in_over);
    ZEXPECT(guessed(over, *in_over) == std::vector<std::string>{"helper(int)"});
};

};  // ZEST_SUITE(SharedHeaderIndex)

}  // namespace

}  // namespace clice::testing
