#include <string>
#include <vector>

#include "test/test.h"
#include "index/search_index.h"
#include "index/symbol_query.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice::testing {
namespace {

using index::SearchEntry;
using index::SearchIndex;
using index::SearchSnapshot;
using index::SymbolFlags;
using index::SymbolHash;
using index::SymbolQuery;

/// A snapshot built row by row; hashes are the row numbers from 1.
struct Corpus {
    SearchSnapshot snapshot;
    llvm::StringMap<SymbolHash> by_qualified;

    SymbolHash add(llvm::StringRef qualified,
                   SymbolKind kind,
                   SymbolFlags flags = SymbolFlags::HasDefinition,
                   std::uint32_t references = 1,
                   llvm::StringRef path = "/w/src/a.cpp",
                   llvm::StringRef args = "") {
        auto [scope, name] = qualified.rsplit("::");
        SymbolHash parent = 0;
        if(!name.empty()) {
            parent = by_qualified.lookup(scope);
        } else {
            name = qualified;
        }
        std::uint32_t file = index::no_file;
        for(std::uint32_t i = 0; i < snapshot.paths.size(); i += 1) {
            if(snapshot.paths[i] == path) {
                file = i;
            }
        }
        if(file == index::no_file) {
            file = static_cast<std::uint32_t>(snapshot.paths.size());
            snapshot.paths.push_back(path.str());
        }
        auto hash = static_cast<SymbolHash>(snapshot.entries.size() + 1);
        snapshot.entries.push_back({
            .hash = hash,
            .name = name.str(),
            .args = args.str(),
            .parent = parent,
            .kind = kind,
            .flags = flags,
            .file = file,
            .reference_files = references,
        });
        by_qualified[(qualified + args).str()] = hash;
        return hash;
    }

    SearchIndex build() {
        SearchIndex built;
        auto bytes = index::build_search_blob(snapshot);
        built.load(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        return built;
    }

    std::string name_of(SymbolHash hash) const {
        auto& entry = snapshot.entries[hash - 1];
        return entry.name + entry.args;
    }
};

Corpus sample() {
    Corpus corpus;
    corpus.add("foo", SymbolKind::Function);
    corpus.add("foobar", SymbolKind::Function);
    corpus.add("xfoo", SymbolKind::Function);
    corpus.add("bar_foo", SymbolKind::Variable);
    corpus.add("Foo", SymbolKind::Struct, SymbolFlags::HasDefinition, 50);
    corpus.add("outer", SymbolKind::Namespace);
    corpus.add("outer::v2",
               SymbolKind::Namespace,
               SymbolFlags::HasDefinition | SymbolFlags::InlineNamespace);
    corpus.add("outer::v2::inner", SymbolKind::Namespace);
    corpus.add("outer::v2::inner::Widget", SymbolKind::Struct);
    corpus.add("outer::v2::inner::Widget::paint", SymbolKind::Method);
    corpus.add("outer::v2::inner::Widget",
               SymbolKind::Struct,
               SymbolFlags::HasDefinition,
               1,
               "/w/src/a.cpp",
               "<int>");
    corpus.add("outer::v2::inner::Widget<int>::paint", SymbolKind::Method);
    corpus.add("outer::versioned", SymbolKind::Function, SymbolFlags::None);
    corpus.add("unique_ptr",
               SymbolKind::Class,
               SymbolFlags::HasDefinition | SymbolFlags::SystemHeader,
               500,
               "/usr/include/memory");
    corpus.add("upper_bound",
               SymbolKind::Function,
               SymbolFlags::HasDefinition,
               3,
               "/w/src/b/algo.h");
    corpus.add("LinkedList", SymbolKind::Struct);
    corpus.add("parse_config", SymbolKind::Function);
    corpus.add("strncpy", SymbolKind::Function);
    corpus.add("strcpy_s", SymbolKind::Function);
    corpus.add("MAX_SIZE", SymbolKind::Macro);
    corpus.add("aaaxaaa", SymbolKind::Function);
    corpus.add("zap_bar_quux", SymbolKind::Function);
    return corpus;
}

std::vector<std::string> names(const Corpus& corpus,
                               const SearchIndex& built,
                               llvm::StringRef text,
                               std::size_t limit = 10) {
    auto query = SymbolQuery::parse(text);
    std::vector<std::string> out;
    if(!query) {
        out.push_back("error: " + query.error());
        return out;
    }
    for(auto& hit: built.search(*query, limit).hits) {
        out.push_back(corpus.name_of(hit.hash));
    }
    return out;
}

using Names = std::vector<std::string>;

TEST_SUITE(SearchIndex) {

TEST_CASE(Loads) {
    auto corpus = sample();
    corpus.snapshot.generation = 7;
    auto built = corpus.build();
    EXPECT_TRUE(built.loaded());
    EXPECT_EQ(built.generation(), std::uint64_t(7));
    EXPECT_EQ(built.size(), corpus.snapshot.entries.size());
    EXPECT_TRUE(built.contains(1));
    EXPECT_FALSE(built.contains(999));
    SearchIndex empty;
    EXPECT_FALSE(empty.load(llvm::MemoryBuffer::getMemBufferCopy("junk")));
    EXPECT_FALSE(empty.loaded());
    EXPECT_TRUE(empty.search(*SymbolQuery::parse("foo"), 10).hits.empty());
    EXPECT_TRUE(built.search(*SymbolQuery::parse("foo"), 100).exhausted);
    EXPECT_FALSE(built.search(*SymbolQuery::parse("foo"), 2).exhausted);
}

TEST_CASE(DamagedPosting) {
    auto corpus = sample();
    auto bytes = index::build_search_blob(corpus.snapshot);
    // Every bitmap image opens with the portable cookie of a bitmap
    // without run containers; breaking each one leaves the blob's
    // structure intact and every posting list undecodable.
    const std::string cookie("\x3a\x30\x00\x00", 4);
    for(auto at = bytes.find(cookie); at != std::string::npos; at = bytes.find(cookie, at + 4)) {
        bytes.replace(at, 4, "\xff\xff\xff\xff");
    }
    SearchIndex built;
    ASSERT_TRUE(built.load(llvm::MemoryBuffer::getMemBufferCopy(bytes)));
    EXPECT_FALSE(built.damaged());
    // The exact matches come from the name order, the fuzzy ones from
    // the posting lists.
    EXPECT_EQ(names(corpus, built, "foo"), (Names{"foo", "Foo"}));
    EXPECT_TRUE(built.damaged());
}

TEST_CASE(FuzzyRanking) {
    auto corpus = sample();
    auto built = corpus.build();
    EXPECT_EQ(names(corpus, built, "foo"), (Names{"foo", "Foo", "foobar", "bar_foo", "xfoo"}));
    EXPECT_EQ(names(corpus, built, "Foo"), (Names{"Foo", "foo", "foobar", "bar_foo", "xfoo"}));
    EXPECT_EQ(names(corpus, built, "LinLis"), (Names{"LinkedList"}));
    EXPECT_EQ(names(corpus, built, "pconf"), (Names{"parse_config"}));
    EXPECT_TRUE(names(corpus, built, "pcfg").empty());
    EXPECT_EQ(names(corpus, built, "up"), (Names{"upper_bound", "unique_ptr"}));
    // A short query keys the first two words in the index and in a row.
    EXPECT_TRUE(names(corpus, built, "qu").empty());
    auto short_query = *SymbolQuery::parse("qu");
    index::NameRanker short_ranker(short_query);
    EXPECT_FALSE(short_ranker.rank("zap_bar_quux", "", 1).has_value());
    EXPECT_TRUE(short_ranker.rank("zap_quux", "", 1).has_value());
    EXPECT_EQ(names(corpus, built, "u_p"), (Names{"unique_ptr"}));
    EXPECT_TRUE(names(corpus, built, "zzz").empty());
    EXPECT_EQ(names(corpus, built, "foo", 2), (Names{"foo", "Foo"}));
}

TEST_CASE(ExactAndGlob) {
    auto corpus = sample();
    auto built = corpus.build();
    EXPECT_EQ(names(corpus, built, R"("foo")"), (Names{"foo"}));
    EXPECT_EQ(names(corpus, built, R"("Foo")"), (Names{"Foo"}));
    EXPECT_TRUE(names(corpus, built, R"("fo")").empty());
    EXPECT_EQ(names(corpus, built, "foo*"), (Names{"Foo", "foo", "foobar"}));
    EXPECT_EQ(names(corpus, built, "*foo"), (Names{"Foo", "bar_foo", "foo", "xfoo"}));
    EXPECT_EQ(names(corpus, built, "str*"), (Names{"strcpy_s", "strncpy"}));
    EXPECT_EQ(names(corpus, built, "*_*"),
              (Names{"unique_ptr",
                     "upper_bound",
                     "bar_foo",
                     "parse_config",
                     "strcpy_s",
                     "zap_bar_quux",
                     "MAX_SIZE"}));
    EXPECT_EQ(names(corpus, built, "MAX_*"), (Names{"MAX_SIZE"}));
    EXPECT_EQ(names(corpus, built, "??o"), (Names{"Foo", "foo"}));
    Corpus wide;
    wide.add((std::string(150, 'a') + "xyz").c_str(), SymbolKind::Function);
    auto wide_index = wide.build();
    EXPECT_EQ(names(wide, wide_index, "*xyz*").size(), std::size_t(1));
}

TEST_CASE(Scopes) {
    auto corpus = sample();
    auto built = corpus.build();
    EXPECT_EQ(names(corpus, built, "inner::paint"), (Names{"paint", "paint"}));
    EXPECT_EQ(names(corpus, built, "outer::inner::paint").size(), std::size_t(2));
    EXPECT_EQ(names(corpus, built, "outer::paint").size(), std::size_t(2));
    EXPECT_TRUE(names(corpus, built, "inner::outer::paint").empty());
    EXPECT_TRUE(names(corpus, built, "::inner::paint").empty());
    EXPECT_TRUE(names(corpus, built, "::outer::inner::paint").empty());
    EXPECT_EQ(names(corpus, built, "::outer::inner::Widget::paint").size(), std::size_t(2));
    EXPECT_EQ(names(corpus, built, "::outer::inner::Widget<int>::paint").size(), std::size_t(1));
    EXPECT_EQ(names(corpus, built, "::outer::versioned"), (Names{"versioned"}));
    EXPECT_EQ(names(corpus, built, "Widget<int>"), (Names{"Widget<int>"}));
    EXPECT_EQ(names(corpus, built, "Widget<int>::paint"), (Names{"paint"}));
    EXPECT_EQ(names(corpus, built, "Widget"), (Names{"Widget", "Widget<int>"}));
    EXPECT_TRUE(names(corpus, built, "v2::Widget").empty());
    EXPECT_EQ(names(corpus, built, "outer::inner::*"), (Names{"Widget", "Widget<int>"}));
    EXPECT_EQ(names(corpus, built, "inner::"), (Names{"Widget", "Widget<int>"}));
    EXPECT_EQ(names(corpus, built, "inner::**"),
              (Names{"Widget", "Widget<int>", "paint", "paint"}));
    EXPECT_EQ(names(corpus, built, "::outer::*"), (Names{"inner", "v2", "versioned"}));
    EXPECT_EQ(names(corpus, built, "::*").size(), std::size_t(10));
    EXPECT_EQ(names(corpus, built, "*", 100).size(), corpus.snapshot.entries.size());
    EXPECT_EQ(names(corpus, built, "::outer::**").size(), std::size_t(7));
    EXPECT_EQ(names(corpus, built, "Widget::*"), (Names{"paint", "paint"}));
    EXPECT_EQ(names(corpus, built, "Widget<int>::*"), (Names{"paint"}));
}

TEST_CASE(Filters) {
    auto corpus = sample();
    auto built = corpus.build();
    EXPECT_EQ(names(corpus, built, "foo kind:struct"), (Names{"Foo"}));
    EXPECT_EQ(names(corpus, built, "foo kind:variable,function"),
              (Names{"foo", "foobar", "bar_foo", "xfoo"}));
    EXPECT_EQ(names(corpus, built, "kind:namespace *"), (Names{"inner", "outer", "v2"}));
    EXPECT_EQ(names(corpus, built, "u path:memory"), (Names{"unique_ptr"}));
    EXPECT_EQ(names(corpus, built, "u path:/usr/"), (Names{"unique_ptr"}));
    EXPECT_EQ(names(corpus, built, "* path:src/b/"), (Names{"upper_bound"}));
    EXPECT_EQ(names(corpus, built, "up path:b/algo.h"), (Names{"upper_bound"}));
    EXPECT_TRUE(names(corpus, built, "up path:nowhere.h").empty());
    EXPECT_TRUE(names(corpus, built, "up kind:macro").empty());
}

TEST_CASE(Typos) {
    auto corpus = sample();
    auto built = corpus.build();
    EXPECT_EQ(names(corpus, built, "strcpy"), (Names{"strcpy_s", "strncpy"}));
    EXPECT_EQ(names(corpus, built, "strcpy", 1), (Names{"strcpy_s"}));
    EXPECT_EQ(names(corpus, built, "strncpx"), (Names{"strncpy"}));
    // A row the clean pass rejected is judged again as a typo.
    EXPECT_EQ(names(corpus, built, "aaaaaa"), (Names{"aaaxaaa"}));
    EXPECT_TRUE(names(corpus, built, "strcp").empty() ||
                names(corpus, built, "strcp") == Names{"strcpy_s"});
}

TEST_CASE(Quality) {
    using index::symbol_quality;
    auto plain = symbol_quality("foo", SymbolKind::Function, SymbolFlags::HasDefinition, 1);
    EXPECT_EQ(plain, 1.0f);
    EXPECT_GT(symbol_quality("foo", SymbolKind::Function, SymbolFlags::HasDefinition, 1000), 1.9f);
    EXPECT_LT(symbol_quality("foo", SymbolKind::Function, SymbolFlags::None, 1), plain);
    EXPECT_LT(symbol_quality("_Foo", SymbolKind::Function, SymbolFlags::HasDefinition, 1), plain);
    EXPECT_LT(symbol_quality("foo", SymbolKind::Macro, SymbolFlags::HasDefinition, 1), plain);
    EXPECT_LT(symbol_quality("foo",
                             SymbolKind::Function,
                             SymbolFlags::HasDefinition | SymbolFlags::SystemHeader,
                             1),
              plain);
    EXPECT_LT(symbol_quality("foo",
                             SymbolKind::Function,
                             SymbolFlags::HasDefinition | SymbolFlags::Deprecated,
                             1),
              plain);
    EXPECT_LT(
        symbol_quality("foo",
                       SymbolKind::Function,
                       index::with_form(SymbolFlags::HasDefinition, index::NameForm::Constructor),
                       1),
        plain);
}

/// The narrowing by tokens and the early stop lose nothing: every hit a
/// full scan of the rows would rank among the best comes out of the
/// index, in the same order.
TEST_CASE(MatchesFullScan) {
    Corpus corpus;
    const char* stems[] = {"get", "set", "parse", "read", "write", "make", "build", "find"};
    const char* tails[] = {"Config", "Value", "Name", "Item", "Node", "Buffer", "Entry", "Path"};
    std::uint32_t references = 1;
    for(auto stem: stems) {
        for(auto tail: tails) {
            references = (references * 7) % 97;
            corpus.add((std::string(stem) + tail).c_str(),
                       SymbolKind::Function,
                       SymbolFlags::HasDefinition,
                       references);
            corpus.add((std::string(stem) + "_" + llvm::StringRef(tail).lower()).c_str(),
                       SymbolKind::Variable,
                       SymbolFlags::HasDefinition,
                       references / 2);
        }
    }
    auto built = corpus.build();
    // Queries under three letters key the first two words only, by
    // design: `e` finds the Entry names, not every name with an e.
    EXPECT_EQ(names(corpus, built, "e", 100).size(), std::size_t(16));
    EXPECT_EQ(names(corpus, built, "gc", 100).size(), std::size_t(2));
    for(llvm::StringRef text:
        {"get", "cfg", "getcon", "readbuf", "Name", "ent", "buf", "path", "setval", "conf"}) {
        auto query = SymbolQuery::parse(text);
        index::NameRanker ranker(*query);

        struct Scored {
            index::NameRank rank;
            std::string name;
            SymbolHash hash;
        };

        std::vector<Scored> all;
        for(auto& entry: corpus.snapshot.entries) {
            auto quality =
                index::symbol_quality(entry.name, entry.kind, entry.flags, entry.reference_files);
            if(auto rank = ranker.rank(entry.name, entry.args, quality, true)) {
                all.push_back({*rank, entry.name, entry.hash});
            }
        }
        std::ranges::sort(all, [](const Scored& lhs, const Scored& rhs) {
            return index::ranks_after(rhs.rank,
                                      rhs.name,
                                      "",
                                      rhs.hash,
                                      lhs.rank,
                                      lhs.name,
                                      "",
                                      lhs.hash);
        });
        std::vector<std::string> expected;
        for(auto& scored: all) {
            if(expected.size() < 5) {
                expected.push_back(scored.name);
            }
        }
        EXPECT_EQ(names(corpus, built, text, 5), expected);
    }
}

};  // TEST_SUITE(SearchIndex)

}  // namespace
}  // namespace clice::testing
