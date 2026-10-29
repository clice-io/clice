module clice;

import :index.columns;
import :index.format;
import :index.search_index;
import :index.symbol_query;
import :support.bitmap;
import :support.bytes;
import :unittests.utils;

namespace clice::testing {
namespace {

using index::SearchEntry;
using index::SearchIndex;
using index::SearchSnapshot;
using index::SymbolFlags;
using index::SymbolHash;
using index::SymbolQuery;

/// A snapshot built entry by entry; hashes are the entry numbers from 1,
/// path ids the positions in `paths`.
struct Corpus {
    SearchSnapshot snapshot;
    std::vector<std::string> paths;
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
        auto file = static_cast<std::uint32_t>(ranges::find(paths, path) - paths.begin());
        if(file == paths.size()) {
            paths.push_back(path.str());
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

    /// The index of one run, its segments offering at most `bound` symbols.
    SearchIndex build(std::uint32_t bound = index::segment_entries) const {
        SearchIndex built;
        built.replace(0, segments(snapshot, bound));
        return built;
    }

    static std::vector<std::unique_ptr<llvm::MemoryBuffer>>
        segments(const SearchSnapshot& snapshot, std::uint32_t bound = index::segment_entries) {
        std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
        for(auto& bytes: index::build_search_segments(snapshot, bound)) {
            buffers.push_back(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
        }
        return buffers;
    }

    /// Whether a file is among a query's paths.
    bool wanted(const SymbolQuery& query, std::uint32_t file) const {
        return ranges::any_of(query.paths, [&](const std::string& wanted) {
            return index::path_matches(wanted, paths[file]);
        });
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
    auto wanted = [&](std::uint32_t file) {
        return corpus.wanted(*query, file);
    };
    for(auto& hit: built.search(*query, limit, wanted).hits) {
        out.push_back(corpus.name_of(hit.hash));
    }
    return out;
}

using Names = std::vector<std::string>;

ZEST_SUITE(SearchIndex) {

ZEST_CASE(Loads) {
    auto corpus = sample();
    auto built = corpus.build();
    ZEXPECT(built.loaded());
    ZEXPECT(built.run_count() == 1u);
    ZEXPECT(built.size() == corpus.snapshot.entries.size());
    SearchIndex empty;
    ZEXPECT(!empty.load(llvm::MemoryBuffer::getMemBufferCopy("junk")));
    ZEXPECT(!empty.loaded());
    ZEXPECT(empty.search(*SymbolQuery::parse("foo"), 10, {}).hits.empty());
    ZEXPECT(built.search(*SymbolQuery::parse("foo"), 100, {}).exhausted);
    ZEXPECT(!built.search(*SymbolQuery::parse("foo"), 2, {}).exhausted);
}

ZEST_CASE(DamagedPosting) {
    auto corpus = sample();
    auto original = std::move(index::build_search_segments(corpus.snapshot).front());
    auto blob = codec::blob::from_bytes<index::SearchIndexBlob>(original);
    ZASSERT(blob);
    // Bytes no posting list starts with: the blob's structure stays
    // intact and every token's list is undecodable.
    ranges::fill(blob->token_postings, 0xff);
    auto bytes = index::encode(*blob);
    SearchIndex built;
    ZASSERT(built.load(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes))));
    ZEXPECT(!built.damaged());
    // The exact matches come from the name order, the fuzzy ones from
    // the posting lists.
    ZEXPECT(names(corpus, built, "foo") == (Names{"foo", "Foo"}));
    ZEXPECT(built.damaged());
}

ZEST_CASE(PostingListRoundTrip) {
    // Empty, single, sparse, dense and full sets, ids at both ends of the
    // universe, and universes too small for low bits.
    auto round_trip = [](std::initializer_list<std::uint32_t> ids, std::uint32_t universe) {
        Bitmap set;
        for(auto id: ids) {
            set.add(id);
        }
        auto decoded = index::read_postings(index::write_postings(set, universe), universe);
        return decoded && *decoded == set;
    };
    ZEXPECT(round_trip({}, 0));
    ZEXPECT(round_trip({}, 100));
    ZEXPECT(round_trip({0}, 1));
    ZEXPECT(round_trip({99}, 100));
    ZEXPECT(round_trip({0, 1, 2, 3}, 4));
    ZEXPECT(round_trip({3, 70000, 70001, 999999}, 1000000));
    Bitmap dense;
    for(std::uint32_t id = 0; id < 50000; id += 3) {
        dense.add(id);
    }
    ZEXPECT(index::read_postings(index::write_postings(dense, 50000), 50000) == dense);
    // A gap of far more than 32 empty high parts.
    Bitmap gapped;
    for(std::uint32_t id = 0; id < 48; id += 1) {
        gapped.add(id);
    }
    gapped.add(999999);
    ZEXPECT(index::read_postings(index::write_postings(gapped, 1000000), 1000000) == gapped);
}

ZEST_CASE(PostingListRejected) {
    // Every byte string but the one write_postings produces is refused:
    // otherwise two would name one set.
    Bitmap set;
    set.add(5);
    set.add(40);
    set.add(99);
    auto list = index::write_postings(set, 100);
    auto rejects = [](std::vector<std::uint8_t> bytes, std::uint32_t universe) {
        return !index::read_postings(bytes, universe).has_value();
    };
    ZASSERT(!rejects(list, 100));
    ZEXPECT(rejects({}, 100));
    ZEXPECT(rejects({list.begin(), list.end() - 1}, 100));
    auto trailing = list;
    trailing.push_back(0);
    ZEXPECT(rejects(trailing, 100));
    auto long_count = list;
    long_count[0] |= 0x80;
    long_count.insert(long_count.begin() + 1, 0x00);
    ZEXPECT(rejects(long_count, 100));
    ZEXPECT(rejects(list, 99));
    ZEXPECT(rejects({0x05}, 4));
    ZEXPECT(rejects({0x00, 0x01}, 100));
    ZEXPECT(rejects({0x80, 0x80, 0x80, 0x80, 0x80, 0x01}, 100));
    // A count of ids taking no low bits, far past the high bits.
    ZEXPECT(rejects({0xff, 0xff, 0xff, 0xff, 0x0f, 0x01}, ~0u));

    // 8 and 9 in a universe of 16: a count, three low bits each, then the
    // high parts.
    Bitmap pair;
    pair.add(8);
    pair.add(9);
    auto low_parts = index::write_postings(pair, 16);
    ZASSERT(low_parts.size() == 3u);
    ZASSERT(!rejects(low_parts, 16));
    low_parts[1] = 0x00;
    ZEXPECT(rejects(low_parts, 16));
    low_parts[1] = 0x48;
    ZEXPECT(rejects(low_parts, 16));
    low_parts[1] = 0x08;
    // The high parts: a third one, a single one, a one past the universe.
    low_parts[2] = 0x07;
    ZEXPECT(rejects(low_parts, 16));
    low_parts[2] = 0x02;
    ZEXPECT(rejects(low_parts, 16));
    low_parts[2] = 0x06;
    ZASSERT(!rejects(low_parts, 16));
    low_parts.push_back(0x01);
    low_parts[2] = 0x02;
    ZEXPECT(rejects(low_parts, 16));
}

ZEST_CASE(FuzzyRanking) {
    auto corpus = sample();
    auto built = corpus.build();
    ZEXPECT(names(corpus, built, "foo") == (Names{"foo", "Foo", "foobar", "bar_foo", "xfoo"}));
    ZEXPECT(names(corpus, built, "Foo") == (Names{"Foo", "foo", "foobar", "bar_foo", "xfoo"}));
    ZEXPECT(names(corpus, built, "LinLis") == (Names{"LinkedList"}));
    ZEXPECT(names(corpus, built, "pconf") == (Names{"parse_config"}));
    ZEXPECT(names(corpus, built, "pcfg").empty());
    ZEXPECT(names(corpus, built, "up") == (Names{"upper_bound", "unique_ptr"}));
    // A short query keys the first two words in the index and in a row.
    ZEXPECT(names(corpus, built, "qu").empty());
    auto short_query = *SymbolQuery::parse("qu");
    index::NameRanker short_ranker(short_query);
    ZEXPECT(!short_ranker.rank("zap_bar_quux", "", 1).has_value());
    ZEXPECT(short_ranker.rank("zap_quux", "", 1));
    ZEXPECT(names(corpus, built, "u_p") == (Names{"unique_ptr"}));
    ZEXPECT(names(corpus, built, "zzz").empty());
    ZEXPECT(names(corpus, built, "foo", 2) == (Names{"foo", "Foo"}));
}

ZEST_CASE(ExactAndGlob) {
    auto corpus = sample();
    auto built = corpus.build();
    ZEXPECT(names(corpus, built, R"("foo")") == (Names{"foo"}));
    ZEXPECT(names(corpus, built, R"("Foo")") == (Names{"Foo"}));
    ZEXPECT(names(corpus, built, R"("fo")").empty());
    ZEXPECT(names(corpus, built, "foo*") == (Names{"Foo", "foo", "foobar"}));
    ZEXPECT(names(corpus, built, "*foo") == (Names{"Foo", "bar_foo", "foo", "xfoo"}));
    ZEXPECT(names(corpus, built, "str*") == (Names{"strcpy_s", "strncpy"}));
    ZEXPECT(names(corpus, built, "*_*") == (Names{"unique_ptr",
                                                  "upper_bound",
                                                  "bar_foo",
                                                  "parse_config",
                                                  "strcpy_s",
                                                  "zap_bar_quux",
                                                  "MAX_SIZE"}));
    ZEXPECT(names(corpus, built, "MAX_*") == (Names{"MAX_SIZE"}));
    ZEXPECT(names(corpus, built, "??o") == (Names{"Foo", "foo"}));
    Corpus wide;
    wide.add((std::string(150, 'a') + "xyz").c_str(), SymbolKind::Function);
    auto wide_index = wide.build();
    ZEXPECT(names(wide, wide_index, "*xyz*").size() == std::size_t(1));
}

ZEST_CASE(Scopes) {
    auto corpus = sample();
    auto built = corpus.build();
    ZEXPECT(names(corpus, built, "inner::paint") == (Names{"paint", "paint"}));
    ZEXPECT(names(corpus, built, "outer::inner::paint").size() == std::size_t(2));
    ZEXPECT(names(corpus, built, "outer::paint").size() == std::size_t(2));
    ZEXPECT(names(corpus, built, "inner::outer::paint").empty());
    ZEXPECT(names(corpus, built, "::inner::paint").empty());
    ZEXPECT(names(corpus, built, "::outer::inner::paint").empty());
    ZEXPECT(names(corpus, built, "::outer::inner::Widget::paint").size() == std::size_t(2));
    ZEXPECT(names(corpus, built, "::outer::inner::Widget<int>::paint").size() == std::size_t(1));
    ZEXPECT(names(corpus, built, "::outer::versioned") == (Names{"versioned"}));
    ZEXPECT(names(corpus, built, "Widget<int>") == (Names{"Widget<int>"}));
    ZEXPECT(names(corpus, built, "Widget<int>::paint") == (Names{"paint"}));
    ZEXPECT(names(corpus, built, "Widget") == (Names{"Widget", "Widget<int>"}));
    ZEXPECT(names(corpus, built, "v2::Widget").empty());
    ZEXPECT(names(corpus, built, "outer::inner::*") == (Names{"Widget", "Widget<int>"}));
    ZEXPECT(names(corpus, built, "inner::") == (Names{"Widget", "Widget<int>"}));
    ZEXPECT(names(corpus, built, "inner::**") ==
            (Names{"Widget", "Widget<int>", "paint", "paint"}));
    ZEXPECT(names(corpus, built, "::outer::*") == (Names{"inner", "v2", "versioned"}));
    ZEXPECT(names(corpus, built, "::*").size() == std::size_t(10));
    ZEXPECT(names(corpus, built, "*", 100).size() == corpus.snapshot.entries.size());
    ZEXPECT(names(corpus, built, "::outer::**").size() == std::size_t(7));
    ZEXPECT(names(corpus, built, "Widget::*") == (Names{"paint", "paint"}));
    ZEXPECT(names(corpus, built, "Widget<int>::*") == (Names{"paint"}));
}

ZEST_CASE(UnnamedScopes) {
    // An unnamed enum's enumerators and an anonymous union's members are
    // named through the enclosing scope, as lookup names them.
    Corpus corpus;
    corpus.add("ns", SymbolKind::Namespace);
    corpus.add("ns::(anonymous enum)",
               SymbolKind::Enum,
               SymbolFlags::HasDefinition | SymbolFlags::Unnamed | SymbolFlags::AnonymousScope);
    corpus.add("ns::(anonymous enum)::kSize", SymbolKind::EnumMember);
    corpus.add("S", SymbolKind::Struct);
    corpus.add("S::(anonymous union)",
               SymbolKind::Union,
               SymbolFlags::HasDefinition | SymbolFlags::Unnamed | SymbolFlags::AnonymousScope);
    corpus.add("S::(anonymous union)::member_a", SymbolKind::Field);
    auto built = corpus.build();
    ZEXPECT(names(corpus, built, "ns::kSize") == (Names{"kSize"}));
    ZEXPECT(names(corpus, built, "::ns::kSize") == (Names{"kSize"}));
    ZEXPECT(names(corpus, built, "::kSize").empty());
    ZEXPECT(names(corpus, built, "S::member_a") == (Names{"member_a"}));
}

ZEST_CASE(Filters) {
    auto corpus = sample();
    auto built = corpus.build();
    ZEXPECT(names(corpus, built, "foo kind:struct") == (Names{"Foo"}));
    ZEXPECT(names(corpus, built, "foo kind:variable,function") ==
            (Names{"foo", "foobar", "bar_foo", "xfoo"}));
    ZEXPECT(names(corpus, built, "kind:namespace *") == (Names{"inner", "outer", "v2"}));
    ZEXPECT(names(corpus, built, "u path:memory") == (Names{"unique_ptr"}));
    ZEXPECT(names(corpus, built, "u path:/usr/") == (Names{"unique_ptr"}));
    ZEXPECT(names(corpus, built, "* path:/w/src/b/") == (Names{"upper_bound"}));
    ZEXPECT(names(corpus, built, "up path:/w/src/b/algo.h") == (Names{"upper_bound"}));
    ZEXPECT(names(corpus, built, "up path:nowhere.h").empty());
    ZEXPECT(names(corpus, built, "up kind:macro").empty());
}

ZEST_CASE(Typos) {
    auto corpus = sample();
    auto built = corpus.build();
    ZEXPECT(names(corpus, built, "strcpy") == (Names{"strcpy_s", "strncpy"}));
    ZEXPECT(names(corpus, built, "strcpy", 1) == (Names{"strcpy_s"}));
    ZEXPECT(names(corpus, built, "strncpx") == (Names{"strncpy"}));
    // An entry the clean pass rejected is judged again as a typo.
    ZEXPECT(names(corpus, built, "aaaaaa") == (Names{"aaaxaaa"}));
    ZEXPECT((names(corpus, built, "strcp").empty() ||
             names(corpus, built, "strcp") == Names{"strcpy_s"}));
}

ZEST_CASE(Quality) {
    using index::symbol_quality;
    auto plain = symbol_quality("foo", SymbolKind::Function, SymbolFlags::HasDefinition, 1);
    ZEXPECT(plain == 1.0f);
    ZEXPECT(symbol_quality("foo", SymbolKind::Function, SymbolFlags::HasDefinition, 1000) > 1.9f);
    ZEXPECT(symbol_quality("foo", SymbolKind::Function, SymbolFlags::None, 1) < plain);
    ZEXPECT(symbol_quality("_Foo", SymbolKind::Function, SymbolFlags::HasDefinition, 1) < plain);
    ZEXPECT(symbol_quality("foo", SymbolKind::Macro, SymbolFlags::HasDefinition, 1) < plain);
    ZEXPECT(symbol_quality("foo",
                           SymbolKind::Function,
                           SymbolFlags::HasDefinition | SymbolFlags::SystemHeader,
                           1) < plain);
    ZEXPECT(symbol_quality("foo",
                           SymbolKind::Function,
                           SymbolFlags::HasDefinition | SymbolFlags::Deprecated,
                           1) < plain);
    ZEXPECT(
        symbol_quality("foo",
                       SymbolKind::Function,
                       index::with_form(SymbolFlags::HasDefinition, index::NameForm::Constructor),
                       1) < plain);
}

/// The narrowing by tokens and the early stop lose nothing: every hit a
/// full scan of the entries would rank among the best comes out of the
/// index, in the same order.
ZEST_CASE(MatchesFullScan) {
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
    ZEXPECT(names(corpus, built, "e", 100).size() == std::size_t(16));
    ZEXPECT(names(corpus, built, "gc", 100).size() == std::size_t(2));
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
        ranges::sort(all, [](const Scored& lhs, const Scored& rhs) {
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
        ZEXPECT(names(corpus, built, text, 5) == expected);
    }
}

ZEST_CASE(LocalHitsNameFile) {
    // A symbol of internal linkage has no symbol record: a hit on it
    // names the file whose shard holds it.
    Corpus corpus;
    corpus.add("helper", SymbolKind::Function);
    corpus.add("helper", SymbolKind::Function, SymbolFlags::HasDefinition, 1, "/w/src/b.cpp");
    corpus.add("helper_too", SymbolKind::Function);
    corpus.snapshot.entries[1].local = true;
    auto built = corpus.build();
    std::vector<std::pair<std::string, std::string>> hits;
    for(auto& hit: built.search(*SymbolQuery::parse("helper"), 10, {}).hits) {
        hits.emplace_back(corpus.name_of(hit.hash),
                          hit.file == index::no_file ? "" : corpus.paths[hit.file]);
    }
    ranges::sort(hits);
    ZEXPECT(hits == (std::vector<std::pair<std::string, std::string>>{
                        {"helper",     ""            },
                        {"helper",     "/w/src/b.cpp"},
                        {"helper_too", ""            },
    }));
}

ZEST_CASE(ExternalOnlyLeavesLocals) {
    // A name of internal linkage ranking first takes no place in a search
    // for external names, however few it asks for.
    Corpus corpus;
    corpus.add("helper", SymbolKind::Function, SymbolFlags::HasDefinition, 9, "/w/src/b.cpp");
    corpus.add("helper_function", SymbolKind::Function);
    corpus.snapshot.entries[0].local = true;
    auto built = corpus.build();
    auto query = *SymbolQuery::parse("helper");
    query.external_only = true;
    std::vector<std::string> names;
    for(auto& hit: built.search(query, 1, {}).hits) {
        names.push_back(corpus.name_of(hit.hash));
    }
    ZEXPECT(names == std::vector<std::string>{"helper_function"});
}

ZEST_CASE(SegmentsAnswerAsOne) {
    // Segments of two symbols each, every one carrying the scopes above
    // its symbols, answer every query as a single segment does.
    auto corpus = sample();
    auto whole = corpus.build();
    auto split = corpus.build(2);
    ZEXPECT(index::build_search_segments(corpus.snapshot, 2).size() == 11u);
    for(llvm::StringRef text: {"foo",
                               "Foo",
                               "up",
                               "u_p",
                               "strcpy",
                               "aaaaaa",
                               R"("foo")",
                               "*foo",
                               "*_*",
                               "inner::paint",
                               "::outer::inner::Widget<int>::paint",
                               "outer::inner::*",
                               "inner::**",
                               "::outer::*",
                               "::*",
                               "::outer::**",
                               "Widget::*",
                               "foo kind:variable,function",
                               "kind:namespace *",
                               "* path:/w/src/b/"}) {
        ZEXPECT(names(corpus, split, text, 100) == names(corpus, whole, text, 100));
    }
    ZEXPECT(names(corpus, split, "foo", 2) == (Names{"foo", "Foo"}));
}

ZEST_CASE(SegmentsBounded) {
    auto corpus = sample();
    std::uint32_t offered = 0;
    for(auto& bytes: index::build_search_segments(corpus.snapshot, 3)) {
        auto blob = codec::blob::from_bytes<index::SearchIndexBlob>(bytes);
        ZASSERT(blob);
        ZEXPECT(blob->qualities.size() <= 3u);
        offered += static_cast<std::uint32_t>(blob->qualities.size());
    }
    ZEXPECT(offered == corpus.snapshot.entries.size());
    // A run with nothing to offer still has a segment, which carries the
    // files it covers.
    SearchSnapshot empty{.covered = {4}};
    auto segments = index::build_search_segments(empty);
    ZASSERT(segments.size() == 1u);
    SearchIndex built;
    ZASSERT(built.replace(0, Corpus::segments(empty)));
    ZEXPECT(built.size() == 0u);
}

ZEST_CASE(NewerRunsHide) {
    // A newer run hides an older one's entry of a symbol it offers, and the
    // older entries of internal linkage in the files it covers.
    Corpus corpus;
    auto foo = corpus.add("foo", SymbolKind::Function);
    corpus.add("helper", SymbolKind::Function, SymbolFlags::HasDefinition, 1, "/w/src/b.cpp");
    corpus.add("other", SymbolKind::Function);
    corpus.snapshot.entries[1].local = true;
    auto built = corpus.build();

    SearchSnapshot change{
        .entries = {{.hash = foo,
                     .name = "food",
                     .kind = SymbolKind::Function,
                     .flags = SymbolFlags::HasDefinition,
                     .file = 0}},
        .covered = {1},
    };
    ZASSERT(built.replace(0, Corpus::segments(change)));
    ZEXPECT(built.run_count() == 2u);
    auto query = *SymbolQuery::parse("foo");
    auto hits = built.search(query, 10, {}).hits;
    ZASSERT(hits.size() == 1u);
    ZEXPECT(hits.front().hash == foo);
    ZEXPECT(built.search(*SymbolQuery::parse("helper"), 10, {}).hits.empty());
    ZEXPECT(built.search(*SymbolQuery::parse("other"), 10, {}).hits.size() == 1u);

    std::vector<SymbolHash> symbols;
    std::vector<std::uint32_t> files;
    built.for_each_source(
        1,
        [&](SymbolHash hash) { symbols.push_back(hash); },
        [&](std::uint32_t file) { files.push_back(file); });
    ZEXPECT(symbols == std::vector<SymbolHash>{foo});
    ZEXPECT(files == std::vector<std::uint32_t>{1});

    // Folding the newer run back leaves the first one whole again.
    SearchSnapshot none;
    ZASSERT(built.replace(1, Corpus::segments(none)));
    ZEXPECT(built.run_count() == 2u);
    ZEXPECT(built.search(*SymbolQuery::parse("helper"), 10, {}).hits.size() == 1u);
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> junk;
    junk.push_back(llvm::MemoryBuffer::getMemBufferCopy("junk"));
    ZEXPECT(!built.replace(1, std::move(junk)));
    ZEXPECT(built.run_count() == 2u);
}

ZEST_CASE(NewerRunsRespellScopes) {
    // A newer run offering a container spelled anew renames it as the
    // scope of the members older runs hold.
    Corpus corpus;
    auto ns = corpus.add("ns", SymbolKind::Namespace);
    auto x = corpus.add("ns::X",
                        SymbolKind::Struct,
                        SymbolFlags::HasDefinition,
                        1,
                        "/w/src/a.cpp",
                        "<LongName>");
    corpus.add("ns::X<LongName>::member", SymbolKind::Method);
    auto built = corpus.build();
    ZASSERT(names(corpus, built, "X<LongName>::member") == (Names{"member"}));

    SearchSnapshot respelled{
        .entries = {{.hash = x,
                     .name = "X",
                     .args = "<int>",
                     .parent = ns,
                     .kind = SymbolKind::Struct,
                     .flags = SymbolFlags::HasDefinition}},
        .scopes = {corpus.snapshot.entries[ns - 1]},
    };
    ZASSERT(built.replace(0, Corpus::segments(respelled)));
    ZEXPECT(names(corpus, built, "X<int>::member") == (Names{"member"}));
    ZEXPECT(names(corpus, built, "::ns::X<int>::*") == (Names{"member"}));
    ZEXPECT(names(corpus, built, "X<LongName>::member").empty());
}

};  // ZEST_SUITE(SearchIndex)

}  // namespace
}  // namespace clice::testing
