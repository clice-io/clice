#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/preamble_state.h"
#include "index/serialization.h"

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
    add_file("forced.h", R"(int §(def)⟦forced_value⟧ = 1;)");
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
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦foo⟧(); }
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
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
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
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

TEST_CASE(MoveConsumedIndex) {
    // The production path (stateless worker) moves the TUIndex into
    // serialize; the blob must be complete even though the index is
    // consumed rather than copied.
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);
    auto foo = hash_of("foo");

    auto blob_path = dir.path("moved.pch.idx");
    std::error_code ec;
    llvm::raw_fd_ostream os(blob_path, ec);
    ASSERT_FALSE(bool(ec));
    index::PreambleState::serialize(*unit, std::move(tu_index), {}, {}, {}, os);
    os.close();

    state = index::PreambleState::load(blob_path);
    ASSERT_TRUE(state != nullptr);

    bool found = false;
    state->lookup(foo,
                  RelationKind::Definition,
                  [&](const index::PreambleState::File& file, const index::Relation& r) {
                      EXPECT_TRUE(file.path.ends_with("foo.h"));
                      EXPECT_EQ(dump(r.range), dump(range("def", "foo.h")));
                      found = true;
                      return false;
                  });
    EXPECT_TRUE(found);

    std::string name;
    SymbolKind kind;
    EXPECT_TRUE(state->find_symbol(foo, name, kind));
    EXPECT_EQ(name, "foo");
}

TEST_CASE(SymbolTableLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_main("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
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
    // PCH pair rebuilds instead of serving a stale layout. The blob only
    // needs the version slot: every other field reads back absent, which is
    // structurally valid — rejection must come from the version check.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    auto blob = kota::codec::fbs::to_bytes(VersionOnly{});
    ASSERT_TRUE(blob.has_value());

    auto blob_path = dir.path("stale.pch.idx");
    dir.touch("stale.pch.idx",
              llvm::StringRef(reinterpret_cast<const char*>(blob->data()), blob->size()));
    EXPECT_TRUE(index::PreambleState::load(blob_path) == nullptr);
}

TEST_CASE(AcceptCurrentVersionBlob) {
    // Positive control for RejectVersionMismatch: the same single-slot shape
    // carrying the CURRENT version loads — slot 0 really is the version slot
    // and the rejection comes from its value, not from the blob's shape.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    auto blob = kota::codec::fbs::to_bytes(VersionOnly{index::preamble_format_version});
    ASSERT_TRUE(blob.has_value());

    dir.touch("current.pch.idx",
              llvm::StringRef(reinterpret_cast<const char*>(blob->data()), blob->size()));
    EXPECT_TRUE(index::PreambleState::load(dir.path("current.pch.idx")) != nullptr);
}

TEST_CASE(RejectCorruptBlob) {
    add_main("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    auto buffer = llvm::MemoryBuffer::getFile(dir.path("state.pch.idx"));
    ASSERT_TRUE(bool(buffer));
    auto bytes = (*buffer)->getBuffer();
    ASSERT_TRUE(bytes.size() > 8);

    dir.touch("truncated.pch.idx", bytes.take_front(bytes.size() / 2));
    EXPECT_TRUE(index::PreambleState::load(dir.path("truncated.pch.idx")) == nullptr);

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    std::string clobbered = bytes.str();
    for(std::size_t i = 4; i < 8; ++i) {
        clobbered[i] = 'X';
    }
    dir.touch("clobbered.pch.idx", clobbered);
    EXPECT_TRUE(index::PreambleState::load(dir.path("clobbered.pch.idx")) == nullptr);
}

TEST_CASE(SourcePathAndContent) {
    add_main("main.cpp", R"(
int value = 42;
int other = 1;
)");
    build_state();

    EXPECT_TRUE(state->source_path().ends_with("main.cpp"));
    EXPECT_EQ(state->preamble_content(), unit->interested_content());
}

};  // TEST_SUITE(PreambleState)

}  // namespace

}  // namespace clice::testing
