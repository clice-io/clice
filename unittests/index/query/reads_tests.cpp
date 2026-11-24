module clice;

import :feature.hierarchy;
import :index.database;
import :index.project_index;
import :index.query;
import :index.query.locate;
import :index.query.text;
import :index.search.locator;
import :project.project;
import :semantic.symbol;
import :unittests.utils;
import :unittests.utils.forwarding_database;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;

namespace clice::testing {
namespace {

ZEST_SUITE(QueryReads) {

TempDir tmp;
IndexerFixture fx;
ReadLog* log = nullptr;

/// Index `source` as main.cpp in a session of its own and save it with its
/// search index; then open the saved index here, logging what is read of
/// it from then on.
void reopen(llvm::StringRef source) {
    tmp.touch("main.cpp", source);
    {
        IndexerFixture first;
        open_store(tmp, first.project);
        ZASSERT(first.merge(index_file(tmp, tmp.path("main.cpp")).data));
        first.save(index::SearchRebuild::Idle);
    }
    open_store(tmp, fx.project);
    auto logged = std::make_unique<ReadLog>(std::move(fx.project.database));
    log = logged.get();
    fx.project.database = std::move(logged);
    ZASSERT(fx.project.index.open(*fx.project.database) == index::LoadResult::Loaded);
    log->reads.clear();
}

const index::IndexQuery& query() const {
    return fx.project.index_query;
}

std::vector<index::IndexQuery::Located> locate(llvm::StringRef text) {
    return index::locate(query(), *index::Locator::parse(text));
}

ZEST_CASE(LocateReadsShardAlone) {
    // Placing a symbol at its definition reads the file's shard alone: not
    // its instantiations, which a walk stopping at the definition never
    // reaches, nor the units depending on the file, whose dead variants a
    // save erased.
    reopen(R"(
        template <typename T>
        T twice(T value) { return value + value; }
        int target() { return twice(1); }
    )");
    ZASSERT(locate("target").size() == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::Shard) == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::InstantiationShard) == 0U);
    ZEXPECT(log->reads_of(index::BlobKind::Dependents) == 0U);
}

ZEST_CASE(ExactNameReadsOne) {
    // A name some symbol spells exactly reads that symbol's record alone,
    // not those of the names it fuzzily matches.
    reopen(R"(
        int GetWorld() { return 1; }
        int GetWorldSettings() { return 2; }
        int GetWorlds() { return 3; }
    )");
    ZASSERT(locate("GetWorld").size() == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::Symbol) == 1U);
}

ZEST_CASE(EnclosingAsksAround) {
    // Which function encloses a reference is asked of the definitions
    // around it alone, not of every definition in the file; it and the
    // line the reference lies on leave the file's instantiations unread.
    reopen(R"(
        int target() { return 1; }
        int first() { return 2; }
        int second() { return 3; }
        int caller() { return target(); }
    )");
    auto located = locate("target");
    ZASSERT(located.size() == 1U);
    auto site = query().first_site(located.front().symbol.hash, {}, RelationKind::Reference);
    ZASSERT(site.has_value());
    log->reads.clear();
    ZEXPECT(feature::enclosing_functions(query(), {*site}) == std::vector<std::string>{"caller"});
    ZEXPECT(index::context_lines(query(), {*site}).size() == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::Symbol) == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::InstantiationShard) == 0U);
}

ZEST_CASE(WalkReadsInstantiations) {
    // A walk going past a file's shard reads its instantiations, and not
    // the units depending on the file.
    reopen(R"(
        struct Item {
            int size() const { return 1; }
        };
        template <typename T>
        int measure(const T& t) { return t.size(); }
        int target() { return measure(Item{}); }
    )");
    auto located = locate("Item::size");
    ZASSERT(located.size() == 1U);
    log->reads.clear();
    ZEXPECT(query().sites(located.front().symbol.hash, {}, RelationKind::Reference).size() == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::InstantiationShard) == 1U);
    ZEXPECT(log->reads_of(index::BlobKind::Dependents) == 0U);
}

};  // ZEST_SUITE(QueryReads)

}  // namespace
}  // namespace clice::testing
