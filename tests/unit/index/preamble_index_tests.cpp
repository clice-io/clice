#include "test/temp_dir.h"
#include "test/test.h"
#include "test/tester.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "sched/workspace.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

namespace {

TEST_SUITE(PreambleIndex, Tester) {

TempDir dir;
std::shared_ptr<index::TUIndex> state;

std::vector<feature::DocumentLink> links;
std::vector<std::uint32_t> inactive;
std::vector<std::uint8_t> conditionals;

/// Compile, build a preamble envelope, persist it as the `.pch.idx` pair
/// and load it back through the production gate.
void build_state(std::source_location location = std::source_location::current()) {
    ASSERT_TRUE(compile());

    links.resize(1);
    links[0].range = {12, 20};
    links[0].target = "/include/foo.h";
    inactive = {4, 9, 30, 42};
    conditionals = {1, 0, 2};

    dir.touch("state.pch.idx", index::build_preamble_index(*unit, links, inactive, conditionals));
    state = load_pch_envelope(dir.path("state.pch.idx"));
    ASSERT_TRUE(state != nullptr);
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    state->iterate_symbols(
        [&](index::SymbolHash symbol_id, const index::SymbolIdentity& symbol, llvm::StringRef) {
            if(symbol.name == name) {
                hash = symbol_id;
                count += 1;
            }
            return true;
        });
    EXPECT_EQ(count, 1);
    return hash;
}

/// Walk a symbol's relation rows in every header section (all but the
/// main file's), the way the query layer's overlay lookup serves them.
void lookup_headers(index::SymbolHash hash,
                    RelationKind kind,
                    llvm::function_ref<bool(llvm::StringRef, const index::Relation&)> callback) {
    for(std::uint32_t i = 0; i < state->section_count(); i += 1) {
        auto path_id = state->section_path(i);
        if(path_id == state->path_count() - 1) {
            continue;
        }
        bool keep = true;
        state->shard_of(path_id).lookup(hash, kind, [&](const index::Relation& r) {
            keep = callback(state->path(path_id), r);
            return keep;
        });
        if(!keep) {
            return;
        }
    }
}

TEST_CASE(ForcedIncludeServed) {
    add_file("forced.h", R"(int §(def)⟦forced_value⟧ = 1;)");
    add_main("main.cpp", R"(int x = forced_value;)");

    // A compile-command forced include: clang records its include edge in
    // the predefines buffer, which is a valid location — so unlike the
    // synthetic buffers themselves, the file must stay in the envelope
    // under its own path.
    prepare();
    owned_args.insert(owned_args.end() - 1, "-include");
    owned_args.insert(owned_args.end() - 1, TestVFS::path("forced.h"));
    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }
    ASSERT_TRUE(try_compile());

    dir.touch("state.pch.idx", index::build_preamble_index(*unit, {}, {}, {}));
    state = load_pch_envelope(dir.path("state.pch.idx"));
    ASSERT_TRUE(state != nullptr);

    bool found = false;
    lookup_headers(hash_of("forced_value"),
                   RelationKind::Definition,
                   [&](llvm::StringRef path, const index::Relation& r) {
                       EXPECT_TRUE(path.ends_with("forced.h"));
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

    // The definition inside the header is served from its section.
    bool found_def = false;
    lookup_headers(foo,
                   RelationKind::Definition,
                   [&](llvm::StringRef path, const index::Relation& r) {
                       EXPECT_TRUE(path.ends_with("foo.h"));
                       EXPECT_EQ(dump(r.range), dump(range("def", "foo.h")));
                       found_def = true;
                       return false;
                   });
    EXPECT_TRUE(found_def);

    // Header-internal references are in the envelope too.
    bool found_ref = false;
    lookup_headers(foo, RelationKind::Reference, [&](llvm::StringRef, const index::Relation& r) {
        if(r.range == range("href", "foo.h")) {
            found_ref = true;
            return false;
        }
        return true;
    });
    EXPECT_TRUE(found_ref);

    // Everything needed to map rows to LSP positions rides in each shard;
    // pure-ASCII content itself is omitted.
    for(std::uint32_t i = 0; i < state->section_count(); i += 1) {
        auto& shard = state->shard_of(state->section_path(i));
        EXPECT_TRUE(shard.content_size() > 0);
        EXPECT_FALSE(shard.line_starts().empty());
        EXPECT_TRUE(shard.ascii());
        EXPECT_TRUE(shard.content().empty());
    }
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
    const index::Shard& preamble = state->shard_of(state->path_count() - 1);
    ASSERT_TRUE(preamble.loaded());

    // Occurrence lookup by offset in the preamble entry.
    bool found_occurrence = false;
    preamble.lookup(point("ref"), [&](const index::Occurrence& occurrence) {
        EXPECT_EQ(occurrence.target, foo);
        EXPECT_EQ(dump(occurrence.range), dump(range("ref")));
        found_occurrence = true;
        return false;
    });
    EXPECT_TRUE(found_occurrence);

    // Relation lookup by symbol in the preamble entry.
    bool found_relation = false;
    preamble.lookup(foo, RelationKind::Reference, [&](const index::Relation& r) {
        EXPECT_EQ(dump(r.range), dump(range("ref")));
        found_relation = true;
        return false;
    });
    EXPECT_TRUE(found_relation);
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

    auto identity = state->find_symbol(foo);
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(identity->name, "foo");
    EXPECT_EQ(identity->kind.value(), SymbolKind(SymbolKind::Function).value());

    EXPECT_FALSE(state->find_symbol(foo + 1).has_value());
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

    // An envelope with no header sections answers lookups with silence,
    // not UB.
    bool visited = false;
    lookup_headers(42, RelationKind::Reference, [&](llvm::StringRef, const index::Relation&) {
        visited = true;
        return true;
    });
    EXPECT_FALSE(visited);
}

TEST_CASE(RejectBadBlob) {
    EXPECT_TRUE(load_pch_envelope(dir.path("missing.pch.idx")) == nullptr);

    dir.touch("garbage.pch.idx", "not a flatbuffer at all");
    EXPECT_TRUE(load_pch_envelope(dir.path("garbage.pch.idx")) == nullptr);
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
    EXPECT_TRUE(load_pch_envelope(blob_path) == nullptr);
}

TEST_CASE(AcceptCurrentVersionBlob) {
    // Positive control for RejectVersionMismatch: the same leading slots
    // carrying the CURRENT version (plus the minimal valid path table, which
    // verification demands) load — slot 0 really is the version slot and the
    // rejection comes from its value, not from the blob's shape.
    struct VersionAndPaths {
        std::uint32_t format_version = 0;
        std::int64_t built_at = 0;
        std::vector<std::string> paths = {"/proj/main.cpp"};
    };

    auto blob =
        kota::codec::fbs::to_bytes(VersionAndPaths{.format_version = index::index_format_version});
    ASSERT_TRUE(blob.has_value());

    dir.touch("current.pch.idx",
              llvm::StringRef(reinterpret_cast<const char*>(blob->data()), blob->size()));
    EXPECT_TRUE(load_pch_envelope(dir.path("current.pch.idx")) != nullptr);
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
    EXPECT_TRUE(load_pch_envelope(dir.path("truncated.pch.idx")) == nullptr);

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    std::string clobbered = bytes.str();
    for(std::size_t i = 4; i < 8; i += 1) {
        clobbered[i] = 'X';
    }
    dir.touch("clobbered.pch.idx", clobbered);
    EXPECT_TRUE(load_pch_envelope(dir.path("clobbered.pch.idx")) == nullptr);
}

TEST_CASE(RejectCorruptSectionBlob) {
    add_main("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    // Overwrite one section's blob bytes in place: the envelope stays
    // structurally valid, but the load gate verifies every blob and must
    // read the pair as missing instead of silently serving nothing.
    auto buffer = llvm::MemoryBuffer::getFile(dir.path("state.pch.idx"));
    ASSERT_TRUE(bool(buffer));
    std::string bytes = (*buffer)->getBuffer().str();

    auto view = index::TUIndex::from_bytes(bytes);
    ASSERT_TRUE(view.loaded());
    ASSERT_TRUE(view.section_count() > 0);
    auto blob = view.section_blob(0);
    auto pos = llvm::StringRef(bytes).find(blob);
    ASSERT_TRUE(pos != llvm::StringRef::npos);
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        bytes[pos + i] = 'X';
    }

    dir.touch("bad_section.pch.idx", bytes);
    EXPECT_TRUE(load_pch_envelope(dir.path("bad_section.pch.idx")) == nullptr);
}

TEST_CASE(SourcePathAndPrefix) {
    add_main("main.cpp", R"(
int value = 42;
int other = 1;
)");
    build_state();

    EXPECT_TRUE(state->path(state->path_count() - 1).ends_with("main.cpp"));

    // The preamble text itself is not stored; the envelope keeps only the
    // identity of the exact prefix it was built from.
    auto content = unit->interested_content();
    EXPECT_TRUE(state->matches_prefix(content));
    EXPECT_TRUE(state->matches_prefix(content.str() + "\nint more = 2;"));
    EXPECT_FALSE(state->matches_prefix(content.drop_back(1)));
    EXPECT_FALSE(state->matches_prefix("int changed = 0;"));

    // An ordinary envelope never serves preamble state.
    auto ordinary = index::build_tu_index(*unit);
    EXPECT_FALSE(index::TUIndex::from_bytes(ordinary).matches_prefix(content));
}

};  // TEST_SUITE(PreambleIndex)

}  // namespace

}  // namespace clice::testing
