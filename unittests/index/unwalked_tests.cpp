module clice;

import :index.symbol;
import :index.tu_index;
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
    // kinds.def is regenerated: its enumeration in kinds.h, the variant of
    // which the receiver stores, holds what the walk of kinds.def records.
    TempDir tmp;
    tmp.touch("kinds.h", R"cpp(#pragma once
enum Kind {
#define KIND(name) name,
#include "kinds.def"
#undef KIND
};
)cpp");
    tmp.touch("kinds.def", "KIND(Alpha)\n");
    tmp.touch("a.cpp", "#include \"kinds.h\"\nKind a() { return Alpha; }\n");
    tmp.touch("b.cpp", "#include \"kinds.h\"\nKind b() { return Beta; }\n");
    auto a_data = index_file(tmp, tmp.path("a.cpp")).data;
    auto a = index::TUIndex::from_bytes(a_data);
    auto header = section_of(a, "kinds.h");
    ZASSERT(header);
    auto known = a.section_identity(*header);

    tmp.touch("kinds.def", "KIND(Alpha)\nKIND(Beta)\n");
    auto walked_data = index_file(tmp, tmp.path("b.cpp")).data;
    auto unwalked_data = index_file(tmp, tmp.path("b.cpp"), {}, {known.variant}, {}, {known}).data;
    auto walked = index::TUIndex::from_bytes(walked_data);
    auto unwalked = index::TUIndex::from_bytes(unwalked_data);
    auto in_walked = section_of(walked, "kinds.def");
    auto in_unwalked = section_of(unwalked, "kinds.def");
    ZASSERT(in_walked);
    ZASSERT(in_unwalked);
    ZEXPECT(unwalked.section_blob(*section_of(unwalked, "kinds.h")).empty());
    ZEXPECT(unwalked.section_hash(*in_unwalked) == walked.section_hash(*in_walked));
};

ZEST_CASE(GuessesStayInClosure) {
    // impl.h names no header: what declares `helper` for its template comes
    // from the file including it. other.h, included before by the unit
    // alone, declares an overload impl.h never sees elsewhere.
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
    ZEXPECT(guessed(view, *impl) == std::vector<std::string>{"helper(int)"});
    auto known = view.section_identity(*impl);
    ZEXPECT(known.reach == 1U);
    ZEXPECT(known.identity != known.own);
};

};  // ZEST_SUITE(UnwalkedHeaders)

}  // namespace

}  // namespace clice::testing
