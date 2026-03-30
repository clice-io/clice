#include "test/test.h"
#include "test/tester.h"
#include "index/project_index.h"

namespace clice::testing {
namespace {

TEST_SUITE(ProjectIndex, Tester) {

bool build_and_index(llvm::StringRef code, index::TUIndex& out) {
    add_main("main.cpp", code);
    if(!compile()) {
        return false;
    }
    out = index::TUIndex::build(*unit);
    return true;
}

TEST_CASE(MergeSingleTU) {
    index::TUIndex tu;
    ASSERT_TRUE(build_and_index(R"(
            int foo() { return 42; }
            int bar() { return foo() + 1; }
        )",
                                tu));

    index::ProjectIndex project;
    auto file_ids_map = project.merge(tu);

    // Path pool should have entries for the TU's files.
    ASSERT_FALSE(project.path_pool.paths.empty());

    // Symbols from the TU should be merged into the project.
    ASSERT_FALSE(project.symbols.empty());

    // Every symbol from the TU should be in the project.
    for(auto& [hash, symbol]: tu.symbols) {
        ASSERT_TRUE(project.symbols.contains(hash));
    }
}

TEST_CASE(MergeMultipleTUs) {
    index::TUIndex tu1;
    ASSERT_TRUE(build_and_index(R"(
            int foo() { return 42; }
        )",
                                tu1));

    index::TUIndex tu2;
    ASSERT_TRUE(build_and_index(R"(
            int bar() { return 99; }
        )",
                                tu2));

    index::ProjectIndex project;
    project.merge(tu1);
    project.merge(tu2);

    // All symbols from both TUs should be present.
    for(auto& [hash, symbol]: tu1.symbols) {
        ASSERT_TRUE(project.symbols.contains(hash));
    }
    for(auto& [hash, symbol]: tu2.symbols) {
        ASSERT_TRUE(project.symbols.contains(hash));
    }
}

TEST_CASE(MergeDuplicateSymbol) {
    // Build two TUs that both define/reference the same function via header.
    add_file("shared.h", R"(
            #pragma once
            inline int shared_func() { return 1; }
        )");
    add_main("a.cpp", R"(
            #include "shared.h"
            int use_a() { return shared_func(); }
        )");
    ASSERT_TRUE(compile());
    auto tu_a = index::TUIndex::build(*unit);

    add_file("shared.h", R"(
            #pragma once
            inline int shared_func() { return 1; }
        )");
    add_main("b.cpp", R"(
            #include "shared.h"
            int use_b() { return shared_func(); }
        )");
    ASSERT_TRUE(compile());
    auto tu_b = index::TUIndex::build(*unit);

    index::ProjectIndex project;
    project.merge(tu_a);
    project.merge(tu_b);

    // Find the shared_func symbol hash from TU A's symbol table.
    index::SymbolHash shared_hash = 0;
    for(auto& [hash, symbol]: tu_a.symbols) {
        if(symbol.name == "shared_func") {
            shared_hash = hash;
            break;
        }
    }
    ASSERT_TRUE(shared_hash != 0);

    // The same hash should exist in project symbols.
    ASSERT_TRUE(project.symbols.contains(shared_hash));

    // reference_files bitmap should contain entries from both TUs.
    auto& proj_sym = project.symbols[shared_hash];
    ASSERT_TRUE(proj_sym.reference_files.cardinality() >= 2U);
}

TEST_CASE(SerializationRoundTrip) {
    index::TUIndex tu;
    ASSERT_TRUE(build_and_index(R"(
            struct Foo { int x; };
            void bar(Foo f) { f.x = 42; }
        )",
                                tu));

    index::ProjectIndex project;
    project.merge(tu);

    // Serialize.
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    project.serialize(os);

    // Deserialize.
    auto restored = index::ProjectIndex::from(buf.data());

    // Path pools should match.
    ASSERT_EQ(project.path_pool.paths.size(), restored.path_pool.paths.size());

    // Symbol tables should have same size.
    ASSERT_EQ(project.symbols.size(), restored.symbols.size());

    // Each symbol should be present in restored with same reference count.
    for(auto& [hash, symbol]: project.symbols) {
        ASSERT_TRUE(restored.symbols.contains(hash));
        auto& restored_sym = restored.symbols[hash];
        ASSERT_EQ(symbol.reference_files.cardinality(), restored_sym.reference_files.cardinality());
    }
}

TEST_CASE(FileIdsMapCorrectness) {
    index::TUIndex tu;
    ASSERT_TRUE(build_and_index(R"(
            int x = 1;
        )",
                                tu));

    index::ProjectIndex project;
    auto file_ids_map = project.merge(tu);

    // file_ids_map should have same size as TU's include graph paths.
    ASSERT_EQ(file_ids_map.size(), tu.graph.paths.size());

    // Each mapped ID should be valid in the project path pool.
    for(auto mapped_id: file_ids_map) {
        ASSERT_TRUE(mapped_id < project.path_pool.paths.size());
    }
}

};  // TEST_SUITE(ProjectIndex)
}  // namespace
}  // namespace clice::testing
