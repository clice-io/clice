#include "test/test.h"
#include "command/command.h"

namespace clice::testing {

namespace {

TEST_SUITE(ToolchainProvider) {

TEST_CASE(InitiallyEmpty) {
    CompilationDatabase cdb;
    EXPECT_FALSE(cdb.has_cached_toolchain());
}

TEST_CASE(InjectResultsPopulatesCache) {
    CompilationDatabase cdb;

    std::vector<ToolchainResult> results;
    results.push_back({
        "key1",
        {"-cc1", "-triple", "x86_64-linux-gnu"}
    });
    cdb.inject_results(results);

    EXPECT_TRUE(cdb.has_cached_toolchain());
}

TEST_CASE(InjectResultsSkipsDuplicateKeys) {
    CompilationDatabase cdb;

    std::vector<ToolchainResult> results;
    results.push_back({
        "key1",
        {"-cc1", "-triple", "x86_64"}
    });
    results.push_back({
        "key1",
        {"-cc1", "-triple", "aarch64"}
    });
    cdb.inject_results(results);

    // After injection, cache should still work.
    EXPECT_TRUE(cdb.has_cached_toolchain());
}

TEST_CASE(GetPendingQueriesReturnsUncachedOnly) {
    CompilationDatabase cdb;

    // Two entries with same flags but different user-content options.
    // They share the same cache key, so only one query is needed.
    CompilationDatabase::PendingEntry entry1;
    entry1.file = "a.cpp";
    entry1.directory = "/tmp";
    entry1.arguments = {"clang++", "-std=c++17", "-DFOO", "a.cpp"};

    CompilationDatabase::PendingEntry entry2;
    entry2.file = "b.cpp";
    entry2.directory = "/tmp";
    entry2.arguments = {"clang++", "-std=c++17", "-DBAR", "b.cpp"};

    auto queries = cdb.get_pending_queries({entry1, entry2});
    // Same driver, same extension, same non-content flags → one query.
    EXPECT_EQ(queries.size(), 1u);
}

TEST_CASE(GetPendingQueriesDeduplicatesSameKey) {
    CompilationDatabase cdb;

    // Three entries with same driver and same flags (only -I/-D differ,
    // which are user-content options excluded from the cache key).
    CompilationDatabase::PendingEntry entry1;
    entry1.file = "x.cpp";
    entry1.directory = "/project";
    entry1.arguments = {"clang++", "-Wall", "-O2", "-DFOO=1", "-I/inc/a", "x.cpp"};

    CompilationDatabase::PendingEntry entry2;
    entry2.file = "y.cpp";
    entry2.directory = "/project";
    entry2.arguments = {"clang++", "-Wall", "-O2", "-DBAR=2", "-I/inc/b", "y.cpp"};

    CompilationDatabase::PendingEntry entry3;
    entry3.file = "z.cpp";
    entry3.directory = "/project";
    entry3.arguments = {"clang++", "-Wall", "-O2", "-Uhello", "z.cpp"};

    auto queries = cdb.get_pending_queries({entry1, entry2, entry3});
    // Same driver, same extension, same non-content flags → same key.
    EXPECT_EQ(queries.size(), 1u);
}

TEST_CASE(GetPendingQueriesDifferentDrivers) {
    CompilationDatabase cdb;

    CompilationDatabase::PendingEntry entry1;
    entry1.file = "a.cpp";
    entry1.directory = "/tmp";
    entry1.arguments = {"clang++", "a.cpp"};

    CompilationDatabase::PendingEntry entry2;
    entry2.file = "b.cpp";
    entry2.directory = "/tmp";
    entry2.arguments = {"g++", "b.cpp"};

    auto queries = cdb.get_pending_queries({entry1, entry2});
    // Different drivers → different keys → two queries.
    EXPECT_EQ(queries.size(), 2u);
}

TEST_CASE(GetPendingQueriesDifferentTargets) {
    CompilationDatabase cdb;

    CompilationDatabase::PendingEntry entry1;
    entry1.file = "a.cpp";
    entry1.directory = "/tmp";
    entry1.arguments = {"clang++", "--target=x86_64-linux-gnu", "a.cpp"};

    CompilationDatabase::PendingEntry entry2;
    entry2.file = "b.cpp";
    entry2.directory = "/tmp";
    entry2.arguments = {"clang++", "--target=aarch64-linux-gnu", "b.cpp"};

    auto queries = cdb.get_pending_queries({entry1, entry2});
    // Different targets → different keys → two queries.
    EXPECT_EQ(queries.size(), 2u);
}

TEST_CASE(GetPendingQueriesDifferentLanguageMode) {
    CompilationDatabase cdb;

    // clang foo.h (default: c-header) vs clang -x c++ foo.h (c++)
    // produce different system include paths, so they must have different keys.
    CompilationDatabase::PendingEntry entry1;
    entry1.file = "foo.h";
    entry1.directory = "/tmp";
    entry1.arguments = {"clang", "foo.h"};

    CompilationDatabase::PendingEntry entry2;
    entry2.file = "foo.h";
    entry2.directory = "/tmp";
    entry2.arguments = {"clang", "-x", "c++", "foo.h"};

    auto queries = cdb.get_pending_queries({entry1, entry2});
    // -x c++ changes language mode → different keys → two queries.
    EXPECT_EQ(queries.size(), 2u);
}

TEST_CASE(GetPendingQueriesSkipsEmptyArgs) {
    CompilationDatabase cdb;

    CompilationDatabase::PendingEntry empty;
    empty.file = "empty.cpp";
    empty.directory = "/tmp";
    // arguments is empty

    CompilationDatabase::PendingEntry valid;
    valid.file = "valid.cpp";
    valid.directory = "/tmp";
    valid.arguments = {"clang++", "valid.cpp"};

    auto queries = cdb.get_pending_queries({empty, valid});
    EXPECT_EQ(queries.size(), 1u);
}

TEST_CASE(InjectThenGetPendingSkipsCached) {
    CompilationDatabase cdb;

    // First, get pending queries to learn what key is generated.
    CompilationDatabase::PendingEntry entry;
    entry.file = "test.cpp";
    entry.directory = "/tmp";
    entry.arguments = {"clang++", "test.cpp"};

    auto queries = cdb.get_pending_queries({entry});
    ASSERT_EQ(queries.size(), 1u);

    // Inject a result for that key.
    std::vector<ToolchainResult> results;
    results.push_back({
        queries[0].key,
        {"-cc1", "-triple", "x86_64-linux-gnu"}
    });
    cdb.inject_results(results);

    // Now the same entry should produce no pending queries.
    auto queries2 = cdb.get_pending_queries({entry});
    EXPECT_EQ(queries2.size(), 0u);
}

};  // TEST_SUITE(ToolchainProvider)

}  // namespace

}  // namespace clice::testing
