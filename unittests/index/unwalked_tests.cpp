module clice;

import :index.symbol;
import :index.tu_index;
import :index.tu_manifest;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(UnwalkedHeaders) {

ZEST_CASE(KnownHeaderLeftUnwalked) {
    TempDir tmp;
    tmp.touch("box.h", R"cpp(#pragma once
template <typename T>
struct Box {
    T value;
    T get() const { return value; }
    friend bool operator==(const Box& a, const Box& b) { return a.value == b.value; }
};
inline int twice(int x) { return x * 2; }
template <typename T>
T unbox(const Box<T>& box) { return box.get(); }
struct Fixed {};
template <typename T>
struct Derived : Fixed, T {};
)cpp");
    tmp.touch("a.cpp", R"cpp(#include "box.h"
int a() { return twice(unbox(Box<int>{1})); }
)cpp");
    tmp.touch("b.cpp", R"cpp(#include "box.h"
struct Tag {};
Derived<Tag> derived;
int b() { return twice(unbox(Box<int>{2})) + unbox(Box<char>{'c'}) + (Box<long>{} == Box<long>{}); }
)cpp");
    auto a_data = index_file(tmp, tmp.path("a.cpp")).data;
    auto a = index::TUIndex::from_bytes(a_data);
    auto header = section_of(a, "box.h");
    ZASSERT(header);
    auto known = a.section_identity(*header);
    ZASSERT(known.identity != 0);

    // The walk of b, its header's variant sent by hash, against b leaving
    // the header unwalked: the same index.
    auto walked_data = index_file(tmp, tmp.path("b.cpp"), {}, {known.variant}).data;
    auto unwalked_data = index_file(tmp, tmp.path("b.cpp"), {}, {known.variant}, {}, {known}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto unwalked = index::TUIndex::from_bytes(unwalked_data);
    ZASSERT(walked.section_count() == unwalked.section_count());
    for(std::uint32_t i = 0; i < walked.section_count(); i += 1) {
        ZEXPECT(walked.path(walked.section_path(i)) == unwalked.path(unwalked.section_path(i)));
        ZEXPECT(walked.section_hash(i) == unwalked.section_hash(i));
        ZEXPECT(walked.section_identity(i) == unwalked.section_identity(i));
    }
    ZASSERT(walked.instantiation_count() == unwalked.instantiation_count());
    for(std::uint32_t i = 0; i < walked.instantiation_count(); i += 1) {
        ZEXPECT(walked.instantiation_blob(i) == unwalked.instantiation_blob(i));
        ZEXPECT(walked.skipped_instantiations(i) == unwalked.skipped_instantiations(i));
    }
    std::vector<std::pair<index::SymbolHash, std::vector<std::uint32_t>>> walked_symbols;
    walked.for_each_symbol(
        [&](index::SymbolHash hash, const index::SymbolView&, llvm::ArrayRef<std::uint32_t> files) {
            walked_symbols.emplace_back(hash, files.vec());
            return true;
        });
    std::vector<std::pair<index::SymbolHash, std::vector<std::uint32_t>>> unwalked_symbols;
    unwalked.for_each_symbol(
        [&](index::SymbolHash hash, const index::SymbolView&, llvm::ArrayRef<std::uint32_t> files) {
            unwalked_symbols.emplace_back(hash, files.vec());
            return true;
        });
    ZEXPECT(walked_symbols == unwalked_symbols);
};

ZEST_CASE(InstantiatedMemberNamed) {
    // The return type of `at` is a typedef of the instantiated class, which
    // no walk of map.h relates: the unit's table names it either way.
    TempDir tmp;
    tmp.touch("map.h", R"cpp(#pragma once
template <class Policy>
struct Map {
    template <class P>
    using Mapped = decltype(P::value());
    template <class P = Policy>
    Mapped<P> at() { return P::value(); }
};
struct Ints {
    static int value() { return 1; }
};
)cpp");
    tmp.touch("a.cpp", "#include \"map.h\"\nint a() { return Map<Ints>{}.at(); }\n");
    tmp.touch("b.cpp", "#include \"map.h\"\nint b() { return Map<Ints>{}.at() + 1; }\n");
    auto a_data = index_file(tmp, tmp.path("a.cpp")).data;
    auto a = index::TUIndex::from_bytes(a_data);
    auto header = section_of(a, "map.h");
    ZASSERT(header);
    auto known = a.section_identity(*header);
    auto walked_data = index_file(tmp, tmp.path("b.cpp"), {}, {known.variant}).data;
    auto unwalked_data = index_file(tmp, tmp.path("b.cpp"), {}, {known.variant}, {}, {known}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto unwalked = index::TUIndex::from_bytes(unwalked_data);
    auto names = [](const index::TUIndex& view) {
        std::vector<std::string> names;
        view.for_each_symbol(
            [&](index::SymbolHash, const index::SymbolView& symbol, llvm::ArrayRef<std::uint32_t>) {
                names.push_back(symbol.name.str());
                return true;
            });
        ranges::sort(names);
        return names;
    };
    ZEXPECT(ranges::contains(names(walked), "Mapped"));
    ZEXPECT(names(unwalked) == names(walked));
};

ZEST_CASE(OtherContextWalked) {
    TempDir tmp;
    tmp.touch("config.h", R"cpp(#pragma once
#ifdef WIDE
using size = long;
#else
using size = int;
#endif
inline size measure() { return 1; }
)cpp");
    tmp.touch("narrow.cpp", "#include \"config.h\"\nsize n = measure();\n");
    tmp.touch("wide.cpp", "#define WIDE\n#include \"config.h\"\nsize w = measure();\n");
    auto narrow_data = index_file(tmp, tmp.path("narrow.cpp")).data;
    auto narrow = index::TUIndex::from_bytes(narrow_data);
    auto header = section_of(narrow, "config.h");
    ZASSERT(header);
    auto known = narrow.section_identity(*header);

    auto wide_data = index_file(tmp, tmp.path("wide.cpp"), {}, {known.variant}, {}, {known}).data;
    auto wide = index::TUIndex::from_bytes(wide_data);
    auto other = section_of(wide, "config.h");
    ZASSERT(other);
    ZEXPECT(wide.section_identity(*other).identity != known.identity);
    ZEXPECT(!wide.section_blob(*other).empty());
};

ZEST_CASE(GeneratedEnumeratorsWalked) {
    // kinds.def lists an enumerator each unit's command names: the
    // enumeration in kinds.h, whose variant the receiver stores, holds what
    // the walk of kinds.def records.
    TempDir tmp;
    tmp.touch("kinds.h", R"cpp(#pragma once
enum Kind {
#define KIND(name) name,
#include "kinds.def"
#undef KIND
};
)cpp");
    tmp.touch("kinds.def", "KIND(Alpha)\nKIND(EXTRA)\n");
    tmp.touch("a.cpp", "#include \"kinds.h\"\nKind a() { return Alpha; }\n");
    tmp.touch("b.cpp", "#include \"kinds.h\"\nKind b() { return Gamma; }\n");
    auto a_data = index_file(tmp, tmp.path("a.cpp"), {"-DEXTRA=Beta"}).data;
    auto a = index::TUIndex::from_bytes(a_data);
    auto header = section_of(a, "kinds.h");
    ZASSERT(header);
    auto known = a.section_identity(*header);

    auto walked_data = index_file(tmp, tmp.path("b.cpp"), {"-DEXTRA=Gamma"}).data;
    auto unwalked_data =
        index_file(tmp, tmp.path("b.cpp"), {"-DEXTRA=Gamma"}, {known.variant}, {}, {known}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto unwalked = index::TUIndex::from_bytes(unwalked_data);
    auto in_walked = section_of(walked, "kinds.def");
    auto in_unwalked = section_of(unwalked, "kinds.def");
    ZASSERT(in_walked);
    ZASSERT(in_unwalked);
    ZEXPECT(unwalked.section_blob(*section_of(unwalked, "kinds.h")).empty());
    ZEXPECT(unwalked.section_hash(*in_unwalked) == walked.section_hash(*in_walked));
};

ZEST_CASE(IncludedHeaderEdited) {
    // b.h gains a better match for a.h's call: a.h, unchanged, is walked
    // again.
    TempDir tmp;
    tmp.touch("b.h", "#pragma once\nint pick(long);\n");
    tmp.touch("a.h", "#pragma once\n#include \"b.h\"\ninline int use() { return pick(0); }\n");
    tmp.touch("main.cpp", "#include \"a.h\"\n");
    auto before_data = index_file(tmp, tmp.path("main.cpp")).data;
    auto before = index::TUIndex::from_bytes(before_data);
    auto header = section_of(before, "a.h");
    ZASSERT(header);
    auto known = before.section_identity(*header);

    tmp.touch("b.h", "#pragma once\nint pick(long);\nint pick(int);\n");
    auto after_data = index_file(tmp, tmp.path("main.cpp"), {}, {}, {}, {known}).data;
    auto after = index::TUIndex::from_bytes(after_data);
    auto again = section_of(after, "a.h");
    ZASSERT(again);
    ZEXPECT(after.section_identity(*again).identity != known.identity);
    ZEXPECT(!after.section_blob(*again).empty());
};

ZEST_CASE(MacroEditedInPlace) {
    // CALL names another function, defined at the same place.
    TempDir tmp;
    tmp.touch("macros.h", "#pragma once\n#define CALL foo\n");
    tmp.touch("use.h", "#pragma once\ninline int use() { return CALL(); }\n");
    tmp.touch("main.cpp", "int foo();\nint bar();\n#include \"macros.h\"\n#include \"use.h\"\n");
    auto before_data = index_file(tmp, tmp.path("main.cpp")).data;
    auto before = index::TUIndex::from_bytes(before_data);
    auto header = section_of(before, "use.h");
    ZASSERT(header);
    auto known = before.section_identity(*header);

    tmp.touch("macros.h", "#pragma once\n#define CALL bar\n");
    auto after_data = index_file(tmp, tmp.path("main.cpp"), {}, {}, {}, {known}).data;
    auto after = index::TUIndex::from_bytes(after_data);
    auto again = section_of(after, "use.h");
    ZASSERT(again);
    ZEXPECT(after.section_identity(*again).identity != known.identity);
};

ZEST_CASE(BodyClosedByInclude) {
    // call()'s body ends in body.inc, which each unit's command fills in.
    TempDir tmp;
    tmp.touch(
        "outer.h",
        "#pragma once\nint first();\nint second();\ninline int call() {\n#include \"body.inc\"\n");
    tmp.touch("body.inc", "return TARGET();\n}\n");
    tmp.touch("main.cpp", "#include \"outer.h\"\n");
    auto a_data = index_file(tmp, tmp.path("main.cpp"), {"-DTARGET=first"}).data;
    auto a = index::TUIndex::from_bytes(a_data);
    auto header = section_of(a, "outer.h");
    ZASSERT(header);
    auto known = a.section_identity(*header);

    auto walked_data = index_file(tmp, tmp.path("main.cpp"), {"-DTARGET=second"}).data;
    auto unwalked_data =
        index_file(tmp, tmp.path("main.cpp"), {"-DTARGET=second"}, {known.variant}, {}, {known})
            .data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto unwalked = index::TUIndex::from_bytes(unwalked_data);
    auto in_walked = section_of(walked, "body.inc");
    auto in_unwalked = section_of(unwalked, "body.inc");
    ZASSERT(in_walked);
    ZASSERT(in_unwalked);
    ZEXPECT(unwalked.section_blob(*section_of(unwalked, "outer.h")).empty());
    ZEXPECT(unwalked.section_hash(*in_unwalked) == walked.section_hash(*in_walked));
};

ZEST_CASE(NamesOfTheIncluder) {
    // body.inc calls the `helper` each unit defines before including it:
    // b's variant of body.inc is b's own.
    TempDir tmp;
    tmp.touch("body.inc", "static int run() { return helper(); }\n");
    tmp.touch("a.cpp", "static int helper() { return 1; }\n#include \"body.inc\"\n");
    tmp.touch("b.cpp", "static int helper() { return 2; }\n#include \"body.inc\"\n");
    auto a_data = index_file(tmp, tmp.path("a.cpp")).data;
    auto a = index::TUIndex::from_bytes(a_data);
    auto body = section_of(a, "body.inc");
    ZASSERT(body);
    auto known = a.section_identity(*body);
    ZEXPECT(known.reach == 1U);

    auto b_data = index_file(tmp, tmp.path("b.cpp"), {}, {known.variant}, {}, {known}).data;
    auto b = index::TUIndex::from_bytes(b_data);
    auto own = section_of(b, "body.inc");
    ZASSERT(own);
    ZEXPECT(b.section_identity(*own).identity != known.identity);
    ZEXPECT(!b.section_blob(*own).empty());
};

ZEST_CASE(GuessesStayInClosure) {
    // impl.h names no header: `helper`, declared by the file including it
    // and by other.h before that, is no candidate it sees on its own, and
    // its identity takes in no includer.
    TempDir tmp;
    tmp.touch("impl.h", R"cpp(#pragma once
template <typename T>
void call(T value) { helper(value); }
)cpp");
    tmp.touch("decl.h", R"cpp(#pragma once
void helper(int);
#include "impl.h"
)cpp");
    tmp.touch("other.h", "#pragma once\nvoid helper(double);\n");
    tmp.touch("main.cpp", "#include \"other.h\"\n#include \"decl.h\"\n");
    auto data = index_file(tmp, tmp.path("main.cpp")).data;
    auto view = index::TUIndex::from_bytes(data);
    auto impl = section_of(view, "impl.h");
    ZASSERT(impl);
    ZEXPECT(guessed(view, *impl).empty());
    auto known = view.section_identity(*impl);
    ZEXPECT(known.reach == 0U);
};

ZEST_CASE(LocalsOfUnwalkedHeaders) {
    // helper has internal linkage and call.h names it: a unit leaving both
    // headers unwalked, its own reindex or another unit, links them as the
    // walk that stored them did.
    TempDir tmp;
    tmp.touch("helper.h", "#pragma once\nstatic int helper() { return 1; }\n");
    tmp.touch("call.h",
              "#pragma once\n#include \"helper.h\"\ninline int call() { return helper(); }\n");
    tmp.touch("a.cpp", "#include \"call.h\"\nint a() { return call(); }\n");
    tmp.touch("b.cpp", "#include \"call.h\"\nint b() { return call(); }\n");
    IndexerFixture f;
    auto& index = f.project.index;
    auto unit = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto links = [&](llvm::StringRef name) {
        auto manifest = index.manifest(unit(name));
        return manifest ? manifest->shared_locals : std::vector<index::SharedLocal>();
    };
    ZASSERT(f.merge(index_file(tmp, tmp.path("a.cpp")).data));
    auto walked = links("a.cpp");
    ZASSERT(walked.size() == 1U);
    ZEXPECT(walked.front().files.size() == 2U);

    auto known = index.known_variants(unit("a.cpp"), {});
    for(auto name: {"a.cpp", "b.cpp"}) {
        auto data = index_file(tmp, tmp.path(name), {}, known.hashes, {}, known.identities).data;
        auto view = index::TUIndex::from_bytes(data);
        for(auto header: {"helper.h", "call.h"}) {
            auto section = section_of(view, header);
            ZASSERT(section);
            ZEXPECT(view.section_blob(*section).empty());
        }
        ZASSERT(f.merge(data));
        ZEXPECT(links(name) == walked);
    }
};

};  // ZEST_SUITE(UnwalkedHeaders)

}  // namespace

}  // namespace clice::testing
