#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "index/tu_index.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

TEST_SUITE(Shard, Tester) {

index::TUIndex tu_index;

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);
}

std::optional<index::SymbolIdentity> lookup_symbol(index::SymbolHash hash) {
    auto it = tu_index.symbols.find(hash);
    if(it == tu_index.symbols.end()) {
        return std::nullopt;
    }
    return index::SymbolIdentity{it->second.name, it->second.kind, it->second.scope};
}

std::string write_fresh(const index::FileIndex& rows,
                        index::RowsHash hash,
                        llvm::StringRef content,
                        bool with_symbols = false) {
    auto resolve = [this](index::SymbolHash symbol) {
        return lookup_symbol(symbol);
    };
    index::VariantInput fresh{hash, &rows, {}};
    if(with_symbols) {
        fresh.symbols = resolve;
    }
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(index::Shard(), {}, fresh, content, llvm::xxh3_64bits(content), os);
    return bytes;
}

std::string append_variant(const index::Shard& old,
                           const index::FileIndex& rows,
                           index::RowsHash hash) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(old,
                       old.variants(),
                       {hash, &rows, {}},
                       old.content(),
                       old.content_hash(),
                       os);
    return bytes;
}

/// Owning wrap: from_bytes borrows, and every builder here returns a
/// temporary string.
index::Shard make_shard(llvm::StringRef bytes) {
    return index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
}

index::SymbolHash hash_at(const index::Shard& shard, std::uint32_t offset) {
    index::SymbolHash result = 0;
    shard.lookup(offset, [&](const index::Occurrence& o) {
        result = o.target;
        return false;
    });
    return result;
}

TEST_CASE(RoundtripLookups) {
    build_index(R"(
        int §(def)⟦§(def)foo⟧() { return 42; }
        int bar() { return §(ref)⟦§(ref)foo⟧(); }
    )");

    auto content = sources.all_files.find("main.cpp")->second.content;
    auto bytes =
        write_fresh(tu_index.main_file_index, tu_index.main_file_index.rows_hash(), content);
    auto shard = make_shard(bytes);
    ASSERT_TRUE(shard.loaded());
    ASSERT_EQ(shard.content(), llvm::StringRef(content));
    ASSERT_FALSE(shard.line_starts().empty());

    auto expected = range("ref");
    bool found = false;
    shard.lookup(point("ref"), [&](const index::Occurrence& o) {
        found = true;
        EXPECT_EQ(o.range.begin, expected.begin);
        return false;
    });
    ASSERT_TRUE(found);

    // The definition relation of the symbol under the reference resolves to
    // the definition site, with the full extent in the payload.
    auto symbol = hash_at(shard, point("ref"));
    ASSERT_TRUE(symbol != 0);
    bool has_definition = false;
    shard.lookup(symbol, RelationKind::Definition, [&](const index::Relation& r) {
        has_definition = true;
        EXPECT_EQ(r.range.begin, range("def").begin);
        return false;
    });
    ASSERT_TRUE(has_definition);
}

index::FileIndex simple_rows(std::initializer_list<index::Occurrence> occurrences) {
    index::FileIndex rows;
    rows.occurrences = occurrences;
    return rows;
}

TEST_CASE(VariantMaskFiltering) {
    auto a = simple_rows({
        {{0, 3}, 111}
    });
    auto b = simple_rows({
        {{0, 3},   111},
        {{10, 13}, 222}
    });

    auto first = make_shard(write_fresh(a, 1, "aaa bbb ccc ddd"));
    auto shard = make_shard(append_variant(first, b, 2));
    ASSERT_TRUE(shard.has_variant(1));
    ASSERT_TRUE(shard.has_variant(2));

    // All variants live by default: both rows serve.
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 11), 222u);

    // Restricting to variant 1 hides the row only variant 2 holds, while
    // the shared row keeps serving.
    shard.set_live({1});
    ASSERT_TRUE(shard.has_dead_variants());
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 11), 0u);

    shard.set_live({1, 2});
    ASSERT_FALSE(shard.has_dead_variants());
    ASSERT_EQ(hash_at(shard, 11), 222u);

    shard.set_live({});
    ASSERT_EQ(hash_at(shard, 1), 0u);
}

TEST_CASE(CompactionDropsVariant) {
    auto a = simple_rows({
        {{0, 3}, 111}
    });
    auto b = simple_rows({
        {{0, 3},   111},
        {{10, 13}, 222}
    });
    auto first = make_shard(write_fresh(a, 1, "aaa bbb ccc ddd"));
    auto both = make_shard(append_variant(first, b, 2));

    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(both, {1}, {}, both.content(), both.content_hash(), os);
    auto compacted = make_shard(bytes);
    ASSERT_TRUE(compacted.has_variant(1));
    ASSERT_FALSE(compacted.has_variant(2));
    ASSERT_EQ(hash_at(compacted, 1), 111u);
    ASSERT_EQ(hash_at(compacted, 11), 0u);
}

/// Grow a shard to `count` variants: variant i holds the shared row plus a
/// unique row at offset i * 16.
index::Shard grow_variants(std::uint32_t count) {
    std::string content(16 * (count + 2), 'x');
    index::Shard shard;
    for(std::uint32_t i = 1; i <= count; i += 1) {
        auto rows = simple_rows({
            {{0, 3},               111     },
            {{i * 16, i * 16 + 3}, 1000 + i}
        });
        shard = make_shard(shard.loaded() ? append_variant(shard, rows, i)
                                          : write_fresh(rows, i, content));
    }
    return shard;
}

void expect_tier_behavior(std::uint32_t count) {
    auto shard = grow_variants(count);
    ASSERT_EQ(shard.variants().size(), std::size_t(count));

    // Every variant's unique row serves under the full live set.
    for(std::uint32_t i = 1; i <= count; i += 1) {
        ASSERT_EQ(hash_at(shard, i * 16 + 1), 1000u + i);
    }

    // One live variant: its unique row and the shared row serve, another
    // variant's unique row does not.
    shard.set_live({3});
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 3 * 16 + 1), 1003u);
    ASSERT_EQ(hash_at(shard, 5 * 16 + 1), 0u);
}

TEST_CASE(MaskTier32) {
    expect_tier_behavior(5);
}

TEST_CASE(MaskTier64) {
    expect_tier_behavior(40);
}

TEST_CASE(MaskTierRoaring) {
    expect_tier_behavior(70);
}

TEST_CASE(LongTokenEscape) {
    auto rows = simple_rows({
        {{0, 300},   111},
        {{400, 404}, 222}
    });
    std::string content(500, 'y');
    auto shard = make_shard(write_fresh(rows, 1, content));

    bool found = false;
    shard.lookup(299, [&](const index::Occurrence& o) {
        found = true;
        EXPECT_EQ(o.range.end, 300u);
        return false;
    });
    ASSERT_TRUE(found);
    ASSERT_EQ(hash_at(shard, 402), 222u);
}

TEST_CASE(RelationPayloadRoundtrip) {
    index::FileIndex rows;
    index::Relation definition{
        .kind = RelationKind::Definition,
        .range = {0, 3}
    };
    definition.set_definition_range({0, 50});
    rows.relations[111] = {
        definition,
        {.kind = RelationKind::Reference, .range = {10, 13}, .target_symbol = 0},
    };
    rows.relations[333] = {
        {.kind = RelationKind::Base, .range = {20, 23}, .target_symbol = 444},
    };

    auto shard = make_shard(write_fresh(rows, 1, std::string(60, 'z')));

    bool checked_definition = false;
    shard.lookup(111, RelationKind::Definition, [&](const index::Relation& r) {
        checked_definition = true;
        auto extent = index::Relation(r).definition_range();
        EXPECT_EQ(extent.begin, 0u);
        EXPECT_EQ(extent.end, 50u);
        return false;
    });
    ASSERT_TRUE(checked_definition);

    bool checked_reference = false;
    shard.lookup(111, RelationKind::Reference, [&](const index::Relation& r) {
        checked_reference = true;
        EXPECT_EQ(r.target_symbol, 0u);
        return false;
    });
    ASSERT_TRUE(checked_reference);

    bool checked_pair = false;
    shard.lookup(333, RelationKind::Base, [&](const index::Relation& r) {
        checked_pair = true;
        EXPECT_EQ(r.target_symbol, 444u);
        return false;
    });
    ASSERT_TRUE(checked_pair);
}

TEST_CASE(LocalSymbolNames) {
    build_index(R"(
        static int §(local)⟦§(local)helper⟧() { return 1; }
        int visible() { return §(use)⟦§(use)helper⟧(); }
    )");

    auto content = sources.all_files.find("main.cpp")->second.content;
    auto shard = make_shard(write_fresh(tu_index.main_file_index,
                                        tu_index.main_file_index.rows_hash(),
                                        content,
                                        /*with_symbols=*/true));

    auto local = hash_at(shard, point("use"));
    ASSERT_TRUE(local != 0);
    std::string name;
    SymbolKind kind;
    ASSERT_TRUE(shard.find_symbol(local, name, kind));
    ASSERT_EQ(name, "helper");

    // External names live in the ProjectIndex, never in the blob.
    auto external = [&] {
        for(auto& [hash, symbol]: tu_index.symbols) {
            if(symbol.name == "visible") {
                return hash;
            }
        }
        return index::SymbolHash(0);
    }();
    ASSERT_TRUE(external != 0);
    ASSERT_FALSE(shard.find_symbol(external, name, kind));
}

TEST_CASE(CorruptBlobRejected) {
    ASSERT_FALSE(index::Shard::from_bytes("not a flatbuffer").loaded());

    // A structurally valid blob of the current version but with no variants
    // is impossible output of the writer, and must not load either.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ASSERT_TRUE(stale.has_value());
    auto data = llvm::StringRef(reinterpret_cast<const char*>(stale->data()), stale->size());
    ASSERT_FALSE(index::Shard::from_bytes(data).loaded());
}

};  // TEST_SUITE(Shard)

}  // namespace
}  // namespace clice::testing
