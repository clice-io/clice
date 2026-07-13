#include "schema_generated.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/preamble_state.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

namespace {

TEST_SUITE(PreambleState, Tester) {

index::TUIndex tu_index;
TempDir dir;
std::shared_ptr<index::PreambleState> state;

std::vector<feature::DocumentLink> links;
std::vector<std::uint32_t> inactive;
std::vector<std::uint8_t> conditionals;

/// Compile, build a full TUIndex, serialize a PreambleState blob to disk
/// and load it back.
void build_state(std::source_location location = std::source_location::current()) {
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    links.resize(1);
    links[0].range = {12, 20};
    links[0].target = "/include/foo.h";
    inactive = {4, 9, 30, 42};
    conditionals = {1, 0, 2};

    auto blob_path = dir.path("state.pch.idx");
    std::error_code ec;
    llvm::raw_fd_ostream os(blob_path, ec);
    ASSERT_FALSE(bool(ec));
    index::PreambleState::serialize(*unit, tu_index, links, inactive, conditionals, os);
    os.close();

    state = index::PreambleState::load(blob_path);
    ASSERT_TRUE(state != nullptr);
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    for(auto& [symbol_id, symbol]: tu_index.symbols) {
        if(symbol.name == name) {
            hash = symbol_id;
            count += 1;
        }
    }
    EXPECT_EQ(count, 1);
    return hash;
}

TEST_CASE(ForcedIncludeServed) {
    add_file("forced.h", R"(int @def[forced_value] = 1;)");
    add_main("main.cpp", R"(int x = forced_value;)");

    // A compile-command forced include: clang records its include edge in
    // the predefines buffer, which is a valid location — so unlike the
    // synthetic buffers themselves, the file must stay in the blob under
    // its own path.
    prepare();
    owned_args.insert(owned_args.end() - 1, "-include");
    owned_args.insert(owned_args.end() - 1, TestVFS::path("forced.h"));
    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }
    ASSERT_TRUE(try_compile());
    tu_index = index::TUIndex::build(*unit);

    auto blob_path = dir.path("state.pch.idx");
    std::error_code ec;
    llvm::raw_fd_ostream os(blob_path, ec);
    ASSERT_FALSE(bool(ec));
    index::PreambleState::serialize(*unit, tu_index, {}, {}, {}, os);
    os.close();

    state = index::PreambleState::load(blob_path);
    ASSERT_TRUE(state != nullptr);

    bool found = false;
    state->lookup(hash_of("forced_value"),
                  RelationKind::Definition,
                  [&](const index::PreambleState::File& file, const index::Relation& r) {
                      EXPECT_TRUE(file.path.ends_with("forced.h"));
                      EXPECT_EQ(dump(r.range), dump(range("def", "forced.h")));
                      found = true;
                      return false;
                  });
    EXPECT_TRUE(found);
}

TEST_CASE(HeaderRelationLookup) {
    add_file("foo.h", R"(
inline void @def[foo]() {}
inline void bar() { @href[foo](); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    // The definition inside the header is served from the blob, together
    // with everything needed to map it to an LSP location.
    bool found_def = false;
    state->lookup(foo,
                  RelationKind::Definition,
                  [&](const index::PreambleState::File& file, const index::Relation& r) {
                      EXPECT_TRUE(file.path.ends_with("foo.h"));
                      EXPECT_FALSE(file.content.empty());
                      EXPECT_FALSE(file.line_starts.empty());
                      EXPECT_EQ(dump(r.range), dump(range("def", "foo.h")));
                      found_def = true;
                      return false;
                  });
    EXPECT_TRUE(found_def);

    // Header-internal references are in the blob too.
    bool found_ref = false;
    state->lookup(foo,
                  RelationKind::Reference,
                  [&](const index::PreambleState::File& file, const index::Relation& r) {
                      if(r.range == range("href", "foo.h")) {
                          found_ref = true;
                          return false;
                      }
                      return true;
                  });
    EXPECT_TRUE(found_ref);
}

TEST_CASE(PreambleLookup) {
    add_file("foo.h", R"(
inline void @def[foo]() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[$(ref)foo](); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    // Occurrence lookup by offset in the preamble entry.
    bool found_occurrence = false;
    state->lookup_preamble(point("ref"), [&](const index::Occurrence& occurrence) {
        EXPECT_EQ(occurrence.target, foo);
        EXPECT_EQ(dump(occurrence.range), dump(range("ref")));
        found_occurrence = true;
        return false;
    });
    EXPECT_TRUE(found_occurrence);

    // Relation lookup by symbol in the preamble entry.
    bool found_relation = false;
    state->lookup_preamble(foo, RelationKind::Reference, [&](const index::Relation& r) {
        EXPECT_EQ(dump(r.range), dump(range("ref")));
        found_relation = true;
        return false;
    });
    EXPECT_TRUE(found_relation);
}

TEST_CASE(SymbolTableLookup) {
    add_file("foo.h", R"(
inline void @def[foo]() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { @ref[foo](); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    std::string name;
    SymbolKind kind;
    ASSERT_TRUE(state->find_symbol(foo, name, kind));
    EXPECT_EQ(name, "foo");
    EXPECT_EQ(kind.value(), SymbolKind(SymbolKind::Function).value());

    EXPECT_FALSE(state->find_symbol(foo + 1, name, kind));
}

TEST_CASE(FeatureStateRoundtrip) {
    add_main("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    auto loaded_links = state->links();
    ASSERT_EQ(loaded_links.size(), 1);
    EXPECT_EQ(loaded_links[0].range, LocalSourceRange(12, 20));
    EXPECT_EQ(loaded_links[0].target, "/include/foo.h");

    EXPECT_EQ(state->inactive_regions(), llvm::ArrayRef<std::uint32_t>(inactive));
    EXPECT_EQ(state->open_conditionals(), llvm::ArrayRef<std::uint8_t>(conditionals));

    // A blob with no header entries answers lookups with silence, not UB.
    bool visited = false;
    state->lookup(42, RelationKind::Reference, [&](auto&, auto&) {
        visited = true;
        return true;
    });
    EXPECT_FALSE(visited);
}

TEST_CASE(RejectBadBlob) {
    EXPECT_TRUE(index::PreambleState::load(dir.path("missing.pch.idx")) == nullptr);

    dir.touch("garbage.pch.idx", "not a flatbuffer at all");
    EXPECT_TRUE(index::PreambleState::load(dir.path("garbage.pch.idx")) == nullptr);
}

TEST_CASE(RejectVersionMismatch) {
    // A structurally valid blob written by a different format version (0 is
    // what a version-less blob reads back) must load as missing, so the
    // PCH pair rebuilds instead of serving a stale layout.
    flatbuffers::FlatBufferBuilder builder(64);
    auto paths = builder.CreateVector(std::vector<flatbuffers::Offset<flatbuffers::String>>{});
    auto files =
        builder.CreateVector(std::vector<flatbuffers::Offset<index::binary::PreambleFileEntry>>{});
    auto symbols = builder.CreateVector(
        std::vector<flatbuffers::Offset<index::binary::PreambleSymbolEntry>>{});
    builder.Finish(index::binary::CreatePreambleState(builder, 0, paths, files, 0, symbols));

    auto blob_path = dir.path("stale.pch.idx");
    dir.touch("stale.pch.idx",
              llvm::StringRef(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                              builder.GetSize()));
    EXPECT_TRUE(index::PreambleState::load(blob_path) == nullptr);
}

};  // TEST_SUITE(PreambleState)

}  // namespace

}  // namespace clice::testing
