module clice;

import :index.symbol;
import :index.tu_index;
import :semantic.symbol;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {

namespace {

/// The section of `file` in a TU index, its position among the sections.
std::optional<std::uint32_t> section_of(const index::TUIndex& view, llvm::StringRef file) {
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        if(view.path(view.section_path(i)).ends_with(file)) {
            return i;
        }
    }
    return std::nullopt;
}

/// The symbols a section names by WeakReference, as `name(args)`.
std::vector<std::string> guessed(const index::TUIndex& view, std::uint32_t section) {
    std::vector<std::string> names;
    view.section_shard(section).for_each_relation(
        [&](index::SymbolHash hash, const index::Relation& relation) {
            if(relation.kind == RelationKind::WeakReference) {
                if(auto symbol = view.find_symbol(hash)) {
                    names.push_back(std::format("{}{}", symbol->name, symbol->args));
                }
            }
            return true;
        });
    ranges::sort(names);
    names.erase(ranges::unique(names).begin(), names.end());
    return names;
}

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
)cpp");
    tmp.touch("a.cpp", R"cpp(#include "box.h"
int a() { return twice(unbox(Box<int>{1})); }
)cpp");
    tmp.touch("b.cpp", R"cpp(#include "box.h"
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
    walked.for_each_symbol([&](index::SymbolHash hash,
                               const index::SymbolView&,
                               llvm::ArrayRef<std::uint32_t> files) {
        walked_symbols.emplace_back(hash, files.vec());
        return true;
    });
    std::vector<std::pair<index::SymbolHash, std::vector<std::uint32_t>>> unwalked_symbols;
    unwalked.for_each_symbol([&](index::SymbolHash hash,
                                 const index::SymbolView&,
                                 llvm::ArrayRef<std::uint32_t> files) {
        unwalked_symbols.emplace_back(hash, files.vec());
        return true;
    });
    ZEXPECT(walked_symbols == unwalked_symbols);
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

ZEST_CASE(GuessesStayInWhatTheHeaderSees) {
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
