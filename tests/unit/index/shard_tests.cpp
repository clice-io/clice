#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "index/tu_index.h"

#include "kota/ipc/lsp/text.h"
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
    tu_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ASSERT_TRUE(tu_index.loaded());
}

std::string write_fresh(const index::FileIndex& rows, llvm::StringRef content) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(rows, {}, content, os);
    return bytes;
}

/// The interested file's worker-encoded blob, straight from the envelope.
std::string main_blob() {
    auto section = tu_index.section_of(tu_index.path_count() - 1);
    if(!section) {
        return {};
    }
    return tu_index.section_blob(*section).str();
}

/// Owning wrap: from_bytes borrows, and every builder here returns a
/// temporary string.
index::Shard make_shard(llvm::StringRef bytes) {
    return index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
}

index::Shard merge(const index::Shard& old,
                   llvm::ArrayRef<index::RowsHash> keep,
                   std::vector<index::Shard> fresh) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::merge_shards(old, keep, fresh, os);
    return make_shard(bytes);
}

/// `content` must repeat the text the shard was built from — ASCII blobs
/// do not store it, so it cannot be recovered from `old`.
index::Shard append_variant(const index::Shard& old,
                            const index::FileIndex& rows,
                            llvm::StringRef content) {
    std::vector<index::Shard> fresh;
    fresh.push_back(make_shard(write_fresh(rows, content)));
    return merge(old, old.variants(), std::move(fresh));
}

index::SymbolHash hash_at(const index::Shard& shard, std::uint32_t offset) {
    index::SymbolHash result = 0;
    shard.lookup(offset, [&](const index::Occurrence& o) {
        result = o.target;
        return false;
    });
    return result;
}

index::FileIndex simple_rows(std::initializer_list<index::Occurrence> occurrences) {
    index::FileIndex rows;
    rows.occurrences = occurrences;
    return rows;
}

TEST_CASE(RoundtripLookups) {
    build_index(R"(
        int §(def)⟦§(def)foo⟧() { return 42; }
        int bar() { return §(ref)⟦§(ref)foo⟧(); }
    )");

    auto content = sources.all_files.find("main.cpp")->second.content;
    auto shard = make_shard(main_blob());
    ASSERT_TRUE(shard.loaded());
    ASSERT_EQ(shard.content_size(), static_cast<std::uint32_t>(content.size()));
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

TEST_CASE(DeterministicEncoding) {
    // The blob's byte hash is the variant's identity, so equal rows must
    // encode to equal bytes regardless of in-memory insertion order.
    index::FileIndex rows;
    rows.occurrences = {
        {{0, 3},   111},
        {{10, 13}, 222},
        {{20, 23}, 111},
    };
    rows.relations[111] = {
        {.kind = RelationKind::Definition, .range = {0, 3},   .target_symbol = 0},
        {.kind = RelationKind::Reference,  .range = {20, 23}, .target_symbol = 0},
    };
    rows.relations[333] = {
        {.kind = RelationKind::Base, .range = {10, 13}, .target_symbol = 444},
    };

    index::FileIndex shuffled;
    shuffled.occurrences = {rows.occurrences[2], rows.occurrences[0], rows.occurrences[1]};
    shuffled.relations[333] = rows.relations[333];
    shuffled.relations[111] = {rows.relations[111][1], rows.relations[111][0]};

    auto content = "aaa bbb ccc ddd 111 222 333";
    ASSERT_EQ(write_fresh(rows, content), write_fresh(shuffled, content));
}

TEST_CASE(AnonymousVariantIdentity) {
    auto rows = simple_rows({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(rows, "aaa bbb");
    auto shard = make_shard(bytes);

    auto variants = shard.variants();
    ASSERT_EQ(variants.size(), std::size_t(1));
    ASSERT_EQ(variants.front(), llvm::xxh3_64bits(bytes));
    ASSERT_TRUE(shard.has_variant(variants.front()));
}

TEST_CASE(AsciiContentOmitted) {
    std::string content = "int x;\nint y;\n";
    auto rows = simple_rows({
        {{4, 5}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));

    ASSERT_TRUE(shard.ascii());
    ASSERT_TRUE(shard.content().empty());
    ASSERT_EQ(shard.content_size(), static_cast<std::uint32_t>(content.size()));
    ASSERT_EQ(shard.content_hash(), llvm::xxh3_64bits(content));
    ASSERT_EQ(hash_at(shard, 4), 111u);

    auto expected = kota::ipc::lsp::build_line_starts(content);
    auto starts = shard.line_starts();
    ASSERT_EQ(std::vector<std::uint32_t>(starts.begin(), starts.end()), expected);
}

TEST_CASE(NonAsciiContentStored) {
    std::string content = "int å;\nint y;\n";
    auto rows = simple_rows({
        {{4, 6}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));

    ASSERT_FALSE(shard.ascii());
    ASSERT_EQ(shard.content(), llvm::StringRef(content));

    auto expected = kota::ipc::lsp::build_line_starts(content);
    auto starts = shard.line_starts();
    ASSERT_EQ(std::vector<std::uint32_t>(starts.begin(), starts.end()), expected);
}

TEST_CASE(LongLineEscape) {
    // A line past 255 bytes escapes to the sparse table; the materialized
    // starts must match a direct scan of the content.
    std::string content = "short\n" + std::string(300, 'a') + "\nshort again\n";
    auto rows = simple_rows({
        {{0, 5}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));

    auto expected = kota::ipc::lsp::build_line_starts(content);
    auto starts = shard.line_starts();
    ASSERT_EQ(std::vector<std::uint32_t>(starts.begin(), starts.end()), expected);
}

TEST_CASE(WideRangeTier) {
    // Past 16MB of content the packed range column cannot hold begins;
    // the wide tier takes over transparently.
    std::string content(index::packed_range_limit + 64, 'w');
    auto rows = simple_rows({
        {{0, 3},                                                          111},
        {{index::packed_range_limit + 8, index::packed_range_limit + 11}, 222},
    });
    auto shard = make_shard(write_fresh(rows, content));
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, index::packed_range_limit + 9), 222u);
}

TEST_CASE(VariantMaskFiltering) {
    auto a = simple_rows({
        {{0, 3}, 111}
    });
    auto b = simple_rows({
        {{0, 3},   111},
        {{10, 13}, 222}
    });

    auto first = make_shard(write_fresh(a, "aaa bbb ccc ddd"));
    auto shard = append_variant(first, b, "aaa bbb ccc ddd");
    auto variants = shard.variants();
    ASSERT_EQ(variants.size(), std::size_t(2));

    // All variants live by default: both rows serve.
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 11), 222u);

    // Restricting to the first variant hides the row only the second
    // holds, while the shared row keeps serving.
    shard.set_live({variants[0]});
    ASSERT_TRUE(shard.has_dead_variants());
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 11), 0u);

    shard.set_live(variants);
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
    auto first = make_shard(write_fresh(a, "aaa bbb ccc ddd"));
    auto both = append_variant(first, b, "aaa bbb ccc ddd");
    auto variants = both.variants();

    auto compacted = merge(both, {variants[0]}, {});
    ASSERT_TRUE(compacted.has_variant(variants[0]));
    ASSERT_FALSE(compacted.has_variant(variants[1]));
    ASSERT_EQ(hash_at(compacted, 1), 111u);
    ASSERT_EQ(hash_at(compacted, 11), 0u);
}

TEST_CASE(KWayMerge) {
    // Several fresh variants land in one write; shared rows collapse with
    // OR-ed masks and each unique row stays filterable to its owner.
    std::string content = "aaa bbb ccc ddd eee";
    std::vector<index::Shard> fresh;
    for(std::uint32_t i = 0; i < 3; i += 1) {
        auto rows = simple_rows({
            {{0, 3},                         111     },
            {{4 * (i + 1), 4 * (i + 1) + 3}, 1000 + i},
        });
        fresh.push_back(make_shard(write_fresh(rows, content)));
    }
    auto shard = merge(index::Shard(), {}, std::move(fresh));

    auto variants = shard.variants();
    ASSERT_EQ(variants.size(), std::size_t(3));
    for(std::uint32_t i = 0; i < 3; i += 1) {
        ASSERT_EQ(hash_at(shard, 4 * (i + 1) + 1), 1000u + i);
    }

    shard.set_live({variants[1]});
    ASSERT_EQ(hash_at(shard, 1), 111u);
    ASSERT_EQ(hash_at(shard, 8 + 1), 1001u);
    ASSERT_EQ(hash_at(shard, 4 + 1), 0u);
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
        auto fresh = make_shard(write_fresh(rows, content));
        if(!shard.loaded()) {
            shard = std::move(fresh);
        } else {
            std::vector<index::Shard> batch;
            batch.push_back(std::move(fresh));
            shard = merge(shard, shard.variants(), std::move(batch));
        }
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
    auto variants = shard.variants();
    ASSERT_EQ(variants.size(), std::size_t(count));

    // Every variant's unique row serves under the full live set, and the
    // shared relation collapsed to one row across all variants.
    for(std::uint32_t i = 1; i <= count; i += 1) {
        ASSERT_EQ(hash_at(shard, i * 16 + 1), 1000u + i);
    }
    ASSERT_EQ(reference_count(shard, 999), std::size_t(count) + 1);

    // One live variant: its unique rows and the shared rows serve, another
    // variant's do not — on the occurrence and the relation side alike.
    shard.set_live({variants[2]});
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
    auto shard = make_shard(write_fresh(rows, content));

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

    auto shard = make_shard(write_fresh(rows, std::string(60, 'z')));

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

    auto shard = make_shard(main_blob());

    auto local = hash_at(shard, point("use"));
    ASSERT_TRUE(local != 0);
    std::string name;
    SymbolKind kind;
    ASSERT_TRUE(shard.find_symbol(local, name, kind));
    ASSERT_EQ(name, "helper");

    // External names live in the ProjectIndex, never in the blob.
    auto external = [&] {
        index::SymbolHash result = 0;
        tu_index.iterate_symbols(
            [&](index::SymbolHash hash, const index::SymbolIdentity& symbol, llvm::StringRef) {
                if(symbol.name == "visible") {
                    result = hash;
                    return false;
                }
                return true;
            });
        return result;
    }();
    ASSERT_TRUE(external != 0);
    ASSERT_FALSE(shard.find_symbol(external, name, kind));
}

TEST_CASE(MergedLocalNames) {
    // Merged blobs carry local names forward from their inputs without any
    // external resolver — every input blob is self-contained.
    build_index(R"(
        static int §(local)⟦§(local)helper⟧() { return 1; }
        int visible() { return helper(); }
    )");
    auto content = sources.all_files.find("main.cpp")->second.content;
    auto first = make_shard(main_blob());

    auto extra = simple_rows({
        {{0, 3}, 424242}
    });
    auto shard = append_variant(first, extra, content);

    auto local = hash_at(shard, point("local"));
    ASSERT_TRUE(local != 0);
    std::string name;
    SymbolKind kind;
    ASSERT_TRUE(shard.find_symbol(local, name, kind));
    ASSERT_EQ(name, "helper");
}

TEST_CASE(WideSymbolIds) {
    // Past 65536 distinct symbols the id columns must widen to u32; a
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
    auto shard = make_shard(write_fresh(rows, content));
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

/// Fill the content identity and line table of a hand-built blob the way
/// the writer would (ASCII omitted, non-ASCII stored).
void fill_content(index::ShardBlob& blob, llvm::StringRef text) {
    blob.content_hash = llvm::xxh3_64bits(text);
    blob.content_size = static_cast<std::uint32_t>(text.size());
    bool is_ascii = llvm::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    blob.content = is_ascii ? std::string() : text.str();
    blob.line_lengths.clear();
    blob.long_line_rows.clear();
    blob.long_line_lengths.clear();
    auto starts = kota::ipc::lsp::build_line_starts(std::string_view(text.data(), text.size()));
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto next = i + 1 < starts.size() ? starts[i + 1] : blob.content_size;
        auto length = next - starts[i];
        if(length >= index::length_escape) {
            blob.line_lengths.push_back(index::length_escape);
            blob.long_line_rows.push_back(static_cast<std::uint32_t>(i));
            blob.long_line_lengths.push_back(length);
        } else {
            blob.line_lengths.push_back(static_cast<std::uint8_t>(length));
        }
    }
}

TEST_CASE(CorruptBlobRejected) {
    ASSERT_FALSE(index::Shard::from_bytes("not a flatbuffer").loaded());

    // A valid blob cut mid-structure must fail verification, not be
    // misread. (One trailing byte can be alignment padding, so the cut
    // must reach real data.)
    auto rows = simple_rows({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(rows, "aaaa");
    ASSERT_FALSE(
        index::Shard::from_bytes(llvm::StringRef(bytes).take_front(bytes.size() / 2)).loaded());

    // A structurally valid table of the current version with no line table
    // at all cannot be writer output.
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
    fill_content(blob, "aaåå");
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

    // Same byte length (ä is two UTF-8 bytes like å): only the hash
    // differs, so the size check cannot be what rejects the blob.
    blob.content = "aaåä";
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(StoredAsciiContentRejected) {
    // The encoding is canonical — one logical blob, one byte image — so
    // pure-ASCII content stored in full is an invalid second spelling of
    // the omitted form.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
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

    blob.content = "aaaa";
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(LineTableMismatchRejected) {
    // Line starts are prefix sums of the length column; a sum drifting off
    // the content size would shift every position mapping below the drift.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaa\nbbb\n");
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

    blob.line_lengths = {4, 3};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    blob.line_lengths = {};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // Redistributing bytes between lines preserves the sum; with stored
    // content every line start must match the one the content derives.
    fill_content(blob, "aå\nbb\n");
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.line_lengths = {3, 4};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(MisorderedRowsRejected) {
    // Occurrence lookup binary-searches decoded row ends; a corrupt blob
    // whose rows lost their order must load as "not on disk" and be
    // rebuilt, not keep misresolving queries on every restart.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occs.packed = {index::pack_range(0, 3), index::pack_range(8, index::length_escape)};
    blob.occs.long_rows = {1};
    blob.occs.long_ends = {12};
    blob.occ_syms8 = {0, 0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // Begins out of order.
    blob.occs.packed = {index::pack_range(8, index::length_escape), index::pack_range(0, 3)};
    blob.occs.long_rows = {0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // Begins sorted, but the escaped end regresses below the row before.
    blob.occs.packed = {index::pack_range(0, index::length_escape), index::pack_range(8, 3)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {14};  // ends decode to {14, 11}
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // An escaped end before its own begin.
    blob.occs.packed = {index::pack_range(0, 3), index::pack_range(8, index::length_escape)};
    blob.occs.long_rows = {1};
    blob.occs.long_ends = {5};  // row 1: begin 8, end 5
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(DuplicateOccKeyRejected) {
    // The merge two-way merges occurrence runs under the full (begin, end,
    // sym) key and the writer combines equal keys, so a repeated or
    // descending symbol under one range is non-canonical and would
    // mis-merge silently instead of being rejected.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111, 222};
    blob.sym_rel_offsets = {0, 0, 0};
    blob.occs.packed = {index::pack_range(0, 3), index::pack_range(0, 3)};
    blob.occ_syms8 = {0, 1};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    // Positive control: one range with ascending symbols loads.
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.occ_syms8 = {0, 0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    blob.occ_syms8 = {1, 0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(UnsortedRelationRowsRejected) {
    // Rows of one relation group merge under the full (kind, begin, end,
    // payload) key and the writer sorts and combines equal keys, so
    // out-of-order or repeated rows are non-canonical and would mis-merge
    // silently instead of being rejected.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 2};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference),
                      static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.rels.packed = {index::pack_range(0, 3), index::pack_range(4, 3)};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.rels.packed = {index::pack_range(4, 3), index::pack_range(0, 3)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    blob.rels.packed = {index::pack_range(0, 3), index::pack_range(0, 3)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(EscapeTableMismatchRejected) {
    // A sentinel length without its sparse entry decodes as begin + 255
    // (end_of's fallback) and a stray entry is silently ignored: with
    // content long enough both pass every range bound and would serve
    // wrong ranges forever, so only the pairing check can reject them.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, std::string(300, 'a'));
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occs.packed = {index::pack_range(0, index::length_escape)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {260};
    blob.occ_syms8 = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // A sentinel without its sparse entry.
    blob.occs.long_rows = {};
    blob.occs.long_ends = {};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // A sparse entry pointing at an unescaped row.
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {260};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // The relation escape table is validated alike.
    blob.occs.packed = {index::pack_range(0, index::length_escape)};
    blob.sym_rel_offsets = {0, 1};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.rels.packed = {index::pack_range(0, index::length_escape)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(RangesBeyondContentRejected) {
    // Every decoded range is served as a source range into the content; an
    // end past it would map positions through text that does not exist —
    // forever, since the blob's content hash still matches the disk and
    // nothing rebuilds it.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 1};
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occ_syms8 = {0};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.rels.packed = {index::pack_range(0, 3)};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // A plain length overruns the 16-byte content.
    blob.occs.packed = {index::pack_range(0, 100)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // An escaped end does too.
    blob.occs.packed = {index::pack_range(0, index::length_escape)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {600};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occs.long_rows = {};
    blob.occs.long_ends = {};

    // Relation ranges are bounded alike.
    blob.rels.packed = {index::pack_range(0, 100)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // Except the no-range sentinel a pair relation legitimately carries —
    // on a source-located kind the same sentinel is corruption.
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    blob.rels.packed = {index::packed_sentinel};
    ASSERT_TRUE(make_shard(bytes_of()).loaded());
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.rels.packed = {index::pack_range(0, 3)};

    // And definition-range payloads.
    blob.rel_def_rows = {0};
    blob.rel_def_begins = {0};
    blob.rel_def_ends = {600};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(WrongRangeTierRejected) {
    // The range tier is a strict function of the content size — a second
    // spelling of the same rows would fork the byte identity.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occs.begins = {0};
    blob.occs.lengths = {3};
    blob.occ_syms8 = {0};

    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::serialize_blob(blob, os);
    ASSERT_FALSE(make_shard(bytes).loaded());
}

TEST_CASE(WrongSymWidthRejected) {
    // The symbol id width is a strict function of the table size, for the
    // same canonicality reason.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occ_syms16 = {0};

    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::serialize_blob(blob, os);
    ASSERT_FALSE(make_shard(bytes).loaded());
}

TEST_CASE(DuplicateSymbolHashRejected) {
    // Symbol lookups lower-bound the hash column and read only the first
    // match's slices: a duplicated hash strands the later id's relations
    // unreachably while the blob keeps loading as fresh.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
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
    // Liveness and compaction select variants by identity; a duplicated
    // entry would make every copy live at once, and rows masked only to the
    // extra id would serve and survive with no contribution owning them.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
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
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 1};
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occ_syms8 = {0};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    blob.rels.packed = {index::pack_range(4, 3)};
    blob.rel_sym_rows = {0};
    blob.rel_sym8 = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.occ_syms8 = {5};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.occ_syms8 = {0};

    blob.rel_sym8 = {5};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(MismatchedPayloadTableRejected) {
    // Readers decode whichever sparse table holds a row without consulting
    // its kind: a decl/def row in the symbol table (or the reverse) would
    // serve one payload's bit pattern as the other.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 1};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Definition)};
    blob.rels.packed = {index::pack_range(4, 3)};
    blob.rel_def_rows = {0};
    blob.rel_def_begins = {0};
    blob.rel_def_ends = {8};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.rel_def_rows = {};
    blob.rel_def_begins = {};
    blob.rel_def_ends = {};
    blob.rel_sym_rows = {0};
    blob.rel_sym8 = {0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.rel_sym_rows = {};
    blob.rel_sym8 = {};
    blob.rel_def_rows = {0};
    blob.rel_def_begins = {0};
    blob.rel_def_ends = {8};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

TEST_CASE(OwnerlessMaskRejected) {
    // A mask owning no stored variant serves its row unconditionally while
    // every variant is live (row_live's live.all fast path never consults
    // it), vanishes once any variant dies, and the next compaction erases
    // it for real — so it must reject the blob at load.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
    blob.variants = {1, 2};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occ_syms8 = {0};
    blob.occs.masks32 = {0b01};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    // An empty mask, then one whose only bit lies past the variant table.
    blob.occs.masks32 = {0};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
    blob.occs.masks32 = {0b100};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // The u64 tier is bounded alike.
    for(std::uint32_t i = 3; i <= 40; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occs.masks32 = {};
    blob.occs.masks64 = {1};
    ASSERT_TRUE(make_shard(bytes_of()).loaded());
    blob.occs.masks64 = {std::uint64_t(1) << 45};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());

    // And roaring masks: decodable but empty, or holding only dropped ids.
    for(std::uint32_t i = 41; i <= 70; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occs.masks64 = {};
    auto set_mask = [&](const clice::Bitmap& mask) {
        blob.occs.roaring.clear();
        for(auto byte: index::write_bitmap(mask)) {
            blob.occs.roaring.push_back(static_cast<std::uint8_t>(byte));
        }
        blob.occs.roaring_offsets = {0, static_cast<std::uint32_t>(blob.occs.roaring.size())};
        blob.rels.roaring_offsets = {0};
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
    fill_content(blob, "aaaa");
    for(std::uint32_t i = 1; i <= 65; i += 1) {
        blob.variants.push_back(i);
    }
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occ_syms8 = {0};

    clice::Bitmap mask;
    mask.add(2);
    for(auto byte: index::write_bitmap(mask)) {
        blob.occs.roaring.push_back(static_cast<std::uint8_t>(byte));
    }
    blob.occs.roaring_offsets = {0, static_cast<std::uint32_t>(blob.occs.roaring.size())};
    blob.rels.roaring_offsets = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ASSERT_TRUE(make_shard(bytes_of()).loaded());

    blob.occs.roaring = {0xff, 0xff, 0xff};
    blob.occs.roaring_offsets = {0, 3};
    ASSERT_FALSE(make_shard(bytes_of()).loaded());
}

};  // TEST_SUITE(Shard)

}  // namespace
}  // namespace clice::testing
