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

/// Grow a shard to `count` variants: variant i holds the shared occurrence
/// and relation plus a unique one of each at offset i * 16.
index::Shard grow_variants(std::uint32_t count) {
    std::string content(16 * (count + 2), 'x');
    index::Shard shard;
    for(std::uint32_t i = 1; i <= count; i += 1) {
        auto rows = simple_rows({
            {{0, 3},               111     },
            {{i * 16, i * 16 + 3}, 1000 + i}
        });
        rows.relations[999] = {
            {.kind = RelationKind::Reference, .range = {0, 3},               .target_symbol = 0},
            {.kind = RelationKind::Reference, .range = {i * 16, i * 16 + 3}, .target_symbol = 0},
        };
        shard = make_shard(shard.loaded() ? append_variant(shard, rows, i)
                                          : write_fresh(rows, i, content));
    }
    return shard;
}

std::size_t reference_count(const index::Shard& shard, index::SymbolHash symbol) {
    std::size_t count = 0;
    shard.lookup(symbol, RelationKind::Reference, [&](const index::Relation&) {
        count += 1;
        return true;
    });
    return count;
}

void expect_tier_behavior(std::uint32_t count) {
    auto shard = grow_variants(count);
    ASSERT_EQ(shard.variants().size(), std::size_t(count));

    // Every variant's unique row serves under the full live set, and the
    // shared relation collapsed to one row across all variants.
    for(std::uint32_t i = 1; i <= count; i += 1) {
        ASSERT_EQ(hash_at(shard, i * 16 + 1), 1000u + i);
    }
    ASSERT_EQ(reference_count(shard, 999), std::size_t(count) + 1);

    // One live variant: its unique rows and the shared rows serve, another
    // variant's do not — on the occurrence and the relation side alike.
    shard.set_live({3});
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 3 * 16 + 1), 1003u);
    ASSERT_EQ(hash_at(shard, 5 * 16 + 1), 0u);
    ASSERT_EQ(reference_count(shard, 999), std::size_t(2));
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

TEST_CASE(WideSymbolIds) {
    // Past 65535 distinct symbols the id columns must widen to u32; a
    // truncating writer corrupts resolution only on indexes this large.
    index::FileIndex rows;
    constexpr std::uint32_t count = 70000;
    rows.occurrences.reserve(count);
    for(std::uint32_t i = 0; i < count; i += 1) {
        rows.occurrences.push_back({
            {i * 8, i * 8 + 3},
            0x100000u + i
        });
    }
    std::string content(count * 8 + 16, 'w');
    auto shard = make_shard(write_fresh(rows, 1, content));
    ASSERT_EQ(hash_at(shard, 69999 * 8 + 1), 0x100000u + 69999);
    ASSERT_EQ(hash_at(shard, 3 * 8 + 1), 0x100000u + 3);
}

TEST_CASE(UnloadedShardNoops) {
    index::Shard shard;
    shard.lookup(0, [&](const index::Occurrence&) { return true; });
    shard.lookup(1, RelationKind::Reference, [&](const index::Relation&) { return true; });
    std::string name;
    SymbolKind kind;
    ASSERT_FALSE(shard.find_symbol(1, name, kind));
    ASSERT_TRUE(shard.content().empty());
    ASSERT_TRUE(shard.line_starts().empty());
}

TEST_CASE(CorruptBlobRejected) {
    ASSERT_FALSE(index::Shard::from_bytes("not a flatbuffer").loaded());

    // A valid blob cut mid-structure must fail verification, not be
    // misread. (One trailing byte can be alignment padding, so the cut
    // must reach real data.)
    auto rows = simple_rows({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(rows, 1, "aaaa");
    ASSERT_FALSE(
        index::Shard::from_bytes(llvm::StringRef(bytes).take_front(bytes.size() / 2)).loaded());

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

TEST_CASE(ContentHashMismatchRejected) {
    // Every freshness decision compares the advertised content hash, so
    // content bytes corrupted under an intact structure would keep loading
    // as fresh while position mapping reads the wrong text.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.content = "aaab";
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(MisorderedRowsRejected) {
    // Occurrence lookup binary-searches decoded row ends; a corrupt blob
    // whose rows lost their order must load as "not on disk" and be
    // rebuilt, not keep misresolving queries on every restart.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaaaaaaaaaaaaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occ_begins = {0, 8};
    blob.occ_lengths = {3, 0xff};  // 0xff escapes to (row, end)
    blob.occ_long_rows = {1};
    blob.occ_long_ends = {12};
    blob.occ_syms16 = {0, 0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // Begins out of order.
    blob.occ_begins = {8, 0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // Begins sorted, but the escaped end regresses below the row before.
    blob.occ_begins = {0, 8};
    blob.occ_lengths = {0xff, 3};
    blob.occ_long_rows = {0};
    blob.occ_long_ends = {14};  // ends decode to {14, 11}
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // An escaped end before its own begin.
    blob.occ_lengths = {3, 0xff};
    blob.occ_long_rows = {1};
    blob.occ_long_ends = {5};  // row 1: begin 8, end 5
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(EscapeTableMismatchRejected) {
    // A sentinel length without its sparse entry decodes as begin + 255
    // (end_of's fallback) and a stray entry is silently ignored: with
    // content long enough both pass every range bound and would serve
    // wrong ranges forever, so only the pairing check can reject them.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = std::string(300, 'a');
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occ_begins = {0};
    blob.occ_lengths = {0xff};
    blob.occ_long_rows = {0};
    blob.occ_long_ends = {260};
    blob.occ_syms16 = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // A sentinel without its sparse entry.
    blob.occ_long_rows = {};
    blob.occ_long_ends = {};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // A sparse entry pointing at an unescaped row.
    blob.occ_lengths = {3};
    blob.occ_long_rows = {0};
    blob.occ_long_ends = {260};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // The relation escape table is validated alike.
    blob.occ_lengths = {0xff};
    blob.sym_rel_offsets = {0, 1};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.rel_begins = {0};
    blob.rel_lengths = {0xff};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(RangesBeyondContentRejected) {
    // Every decoded range is served as a source range into the stored
    // content; an end past it would map positions through text that does
    // not exist — forever, since the blob's content hash still matches the
    // disk and nothing rebuilds it.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaaaaaaaaaaaaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 1};
    blob.occ_begins = {0};
    blob.occ_lengths = {3};
    blob.occ_syms16 = {0};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.rel_begins = {0};
    blob.rel_lengths = {3};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // A plain length overruns the 16-byte content.
    blob.occ_lengths = {100};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // An escaped end does too.
    blob.occ_lengths = {0xff};
    blob.occ_long_rows = {0};
    blob.occ_long_ends = {600};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.occ_lengths = {3};
    blob.occ_long_rows = {};
    blob.occ_long_ends = {};

    // Relation ranges are bounded alike.
    blob.rel_lengths = {100};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // Except the no-range sentinel a pair relation legitimately carries —
    // on a source-located kind the same sentinel is corruption.
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    blob.rel_begins = {0xffffffff};
    blob.rel_lengths = {0};
    ASSERT_TRUE(make_shard(bytes_of()).loaded());
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.rel_begins = {0};
    blob.rel_lengths = {3};

    // And definition-range payloads.
    blob.rel_def_rows = {0};
    blob.rel_def_begins = {0};
    blob.rel_def_ends = {600};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(DuplicateSymbolHashRejected) {
    // Symbol lookups lower-bound the hash column and read only the first
    // match's slices: a duplicated hash strands the later id's relations
    // unreachably while the blob keeps loading as fresh.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1};
    blob.sym_hashes = {111, 222};
    blob.sym_rel_offsets = {0, 0, 0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.sym_hashes = {111, 111};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(DuplicateVariantRejected) {
    // Liveness and compaction select variants by rows hash; a duplicated
    // entry would make every copy live at once, and rows masked only to the
    // extra id would serve and survive with no contribution owning them.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1, 2};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.variants = {1, 1};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(StraySymbolIdRejected) {
    // Lookups dereference symbol ids straight into the hash table; an id
    // past it would previously read as "no symbol", missing the occurrence
    // or dropping the relation's target forever with no reindex triggered.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaaaaaaaaaaaaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 1};
    blob.occ_begins = {0};
    blob.occ_lengths = {3};
    blob.occ_syms16 = {0};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    blob.rel_begins = {4};
    blob.rel_lengths = {3};
    blob.rel_sym_rows = {0};
    blob.rel_sym16 = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.occ_syms16 = {5};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.occ_syms16 = {0};

    blob.rel_sym16 = {5};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(OwnerlessMaskRejected) {
    // A mask owning no stored variant serves its row unconditionally while
    // every variant is live (row_live's live.all fast path never consults
    // it), vanishes once any variant dies, and the next compaction erases
    // it for real — so it must reject the blob at load.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    blob.variants = {1, 2};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occ_begins = {0};
    blob.occ_lengths = {3};
    blob.occ_syms16 = {0};
    blob.occ_masks32 = {0b01};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // An empty mask, then one whose only bit lies past the variant table.
    blob.occ_masks32 = {0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.occ_masks32 = {0b100};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // The u64 tier is bounded alike.
    for(std::uint32_t i = 3; i <= 40; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occ_masks32 = {};
    blob.occ_masks64 = {1};
    ASSERT_TRUE(make_shard(bytes_of()).loaded());
    blob.occ_masks64 = {std::uint64_t(1) << 45};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // And roaring masks: decodable but empty, or holding only dropped ids.
    for(std::uint32_t i = 41; i <= 70; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occ_masks64 = {};
    auto set_mask = [&](const clice::Bitmap& mask) {
        blob.occ_roaring.clear();
        for(auto byte: index::write_bitmap(mask)) {
            blob.occ_roaring.push_back(static_cast<std::uint8_t>(byte));
        }
        blob.occ_roaring_offsets = {0, static_cast<std::uint32_t>(blob.occ_roaring.size())};
        blob.rel_roaring_offsets = {0};
    };
    clice::Bitmap in_range;
    in_range.add(69);
    set_mask(in_range);
    ASSERT_TRUE(make_shard(bytes_of()).loaded());
    set_mask({});
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    clice::Bitmap stray;
    stray.add(70);
    set_mask(stray);
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(CorruptRoaringMaskRejected) {
    // Roaring row masks gate liveness and are rewritten by compaction; a
    // slice failing decode would read the row as dead and the next
    // compaction would erase it for real, every manifest still fresh — so
    // an undecodable slice must reject the blob at load.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content = "aaaa";
    blob.content_hash = llvm::xxh3_64bits(llvm::StringRef(blob.content));
    for(std::uint32_t i = 1; i <= 65; i += 1) {
        blob.variants.push_back(i);
    }
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occ_begins = {0};
    blob.occ_lengths = {3};
    blob.occ_syms16 = {0};

    clice::Bitmap mask;
    mask.add(2);
    for(auto byte: index::write_bitmap(mask)) {
        blob.occ_roaring.push_back(static_cast<std::uint8_t>(byte));
    }
    blob.occ_roaring_offsets = {0, static_cast<std::uint32_t>(blob.occ_roaring.size())};
    blob.rel_roaring_offsets = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.occ_roaring = {0xff, 0xff, 0xff};
    blob.occ_roaring_offsets = {0, 3};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

};  // TEST_SUITE(Shard)

}  // namespace
}  // namespace clice::testing
