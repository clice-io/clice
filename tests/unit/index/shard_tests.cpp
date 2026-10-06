module;

#include "modules/prelude.h"

module clice;

import :index.serialization;
import :index.shard;
import :index.site;
import :index.tu_index;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {
namespace {

ZEST_SUITE(Shard, Tester) {

index::TUIndex tu_index;

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ZASSERT(compile());
    tu_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ZASSERT(tu_index.loaded());
}

std::string write_fresh(const index::FileIndex& rows, llvm::StringRef content) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard(rows, {}, content, os);
    return bytes;
}

/// The main file's worker-encoded blob, straight from the envelope.
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

ZEST_CASE(RoundtripLookups) {
    build_index(R"(
        int §(def)⟦§(def)foo⟧() { return 42; }
        int bar() { return §(ref)⟦§(ref)foo⟧(); }
    )");

    auto content = sources.all_files.find("main.cpp")->second.content;
    auto shard = make_shard(main_blob());
    ZASSERT(shard.loaded());
    ZASSERT(shard.content_size() == static_cast<std::uint32_t>(content.size()));
    ZASSERT(!shard.line_starts().empty());

    auto expected = range("ref");
    bool found = false;
    shard.lookup(point("ref"), [&](const index::Occurrence& o) {
        found = true;
        ZEXPECT(o.range.begin == expected.begin);
        return false;
    });
    ZASSERT(found);

    // The definition relation of the symbol under the reference resolves to
    // the definition site, with the full extent in the payload.
    auto symbol = hash_at(shard, point("ref"));
    ZASSERT(symbol != 0);
    bool has_definition = false;
    shard.lookup(symbol, RelationKind::Definition, [&](const index::Relation& r) {
        has_definition = true;
        ZEXPECT(r.range.begin == range("def").begin);
        return false;
    });
    ZASSERT(has_definition);
}

ZEST_CASE(DeterministicEncoding) {
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
    ZASSERT(write_fresh(rows, content) == write_fresh(shuffled, content));
}

ZEST_CASE(AnonymousVariantIdentity) {
    auto rows = simple_rows({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(rows, "aaa bbb");
    auto shard = make_shard(bytes);

    auto variants = shard.variants();
    ZASSERT(variants.size() == std::size_t(1));
    ZASSERT(variants.front() == llvm::xxh3_64bits(bytes));
    ZASSERT(shard.has_variant(variants.front()));
}

ZEST_CASE(AsciiContentOmitted) {
    std::string content = "int x;\nint y;\n";
    auto rows = simple_rows({
        {{4, 5}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));

    ZASSERT(shard.content().empty());
    ZASSERT(shard.content_size() == static_cast<std::uint32_t>(content.size()));
    ZASSERT(shard.content_hash() == llvm::xxh3_64bits(content));
    ZASSERT(hash_at(shard, 4) == 111u);

    auto expected = kota::ipc::lsp::line_starts(content);
    auto starts = shard.line_starts();
    ZASSERT(std::vector<std::uint32_t>(starts.begin(), starts.end()) == expected);
}

ZEST_CASE(CRLFLinesMarked) {
    // ASCII content is not stored: the shard marks the lines ending in
    // "\r\n" so its coordinates place their ends without the text.
    std::string content = "int x;\r\nint y;\nint z;\r\n";
    auto rows = simple_rows({
        {{4, 5}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));
    ZASSERT(shard.content().empty());
    auto more = simple_rows({
        {{12, 13}, 222}
    });
    auto merged = append_variant(shard, more, content);
    ZASSERT(std::ranges::equal(merged.crlf_lines(), shard.crlf_lines()));

    auto starts = shard.line_starts();
    index::Coordinates marked(shard.content_size(), starts, shard.crlf_lines());
    index::Coordinates scanned(content, starts);
    for(std::uint32_t row = 0; row < starts.size(); row += 1) {
        ZEXPECT(marked.line_bounds(row) == scanned.line_bounds(row));
    }
}

ZEST_CASE(NonAsciiContentStored) {
    std::string content = "int å;\nint y;\n";
    auto rows = simple_rows({
        {{4, 6}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));

    ZASSERT(shard.content() == llvm::StringRef(content));

    auto expected = kota::ipc::lsp::line_starts(content);
    auto starts = shard.line_starts();
    ZASSERT(std::vector<std::uint32_t>(starts.begin(), starts.end()) == expected);
}

ZEST_CASE(LongLineEscape) {
    // A line past 255 bytes escapes to the sparse table; the materialized
    // starts must match a direct scan of the content.
    std::string content = "short\n" + std::string(300, 'a') + "\nshort again\n";
    auto rows = simple_rows({
        {{0, 5}, 111}
    });
    auto shard = make_shard(write_fresh(rows, content));

    auto expected = kota::ipc::lsp::line_starts(content);
    auto starts = shard.line_starts();
    ZASSERT(std::vector<std::uint32_t>(starts.begin(), starts.end()) == expected);
}

ZEST_CASE(WideRangeTier) {
    // Past 16MB of content the packed range column cannot hold begins;
    // the wide tier takes over transparently.
    std::string content(index::packed_range_limit + 64, 'w');
    auto rows = simple_rows({
        {{0, 3},                                                          111},
        {{index::packed_range_limit + 8, index::packed_range_limit + 11}, 222},
    });
    auto shard = make_shard(write_fresh(rows, content));
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, index::packed_range_limit + 9) == 222u);
}

ZEST_CASE(VariantMaskFiltering) {
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
    ZASSERT(variants.size() == std::size_t(2));

    // All variants live by default: both rows serve.
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, 11) == 222u);

    // Restricting to the first variant hides the row only the second
    // holds, while the shared row keeps serving.
    shard.set_live({variants[0]});
    ZASSERT(shard.has_dead_variants());
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, 11) == 0u);

    shard.set_live(variants);
    ZASSERT(!shard.has_dead_variants());
    ZASSERT(hash_at(shard, 11) == 222u);

    shard.set_live({});
    ZASSERT(hash_at(shard, 1) == 0u);
}

ZEST_CASE(CompactionDropsVariant) {
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
    ZASSERT(compacted.has_variant(variants[0]));
    ZASSERT(!compacted.has_variant(variants[1]));
    ZASSERT(hash_at(compacted, 1) == 111u);
    ZASSERT(hash_at(compacted, 11) == 0u);
}

ZEST_CASE(KWayMerge) {
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
    ZASSERT(variants.size() == std::size_t(3));
    for(std::uint32_t i = 0; i < 3; i += 1) {
        ZASSERT(hash_at(shard, 4 * (i + 1) + 1) == 1000u + i);
    }

    shard.set_live({variants[1]});
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, 8 + 1) == 1001u);
    ZASSERT(hash_at(shard, 4 + 1) == 0u);
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
    ZASSERT(variants.size() == std::size_t(count));

    // Every variant's unique row serves under the full live set, and the
    // shared relation collapsed to one row across all variants.
    for(std::uint32_t i = 1; i <= count; i += 1) {
        ZASSERT(hash_at(shard, i * 16 + 1) == 1000u + i);
    }
    ZASSERT(reference_count(shard, 999) == std::size_t(count) + 1);

    // One live variant: its unique rows and the shared rows serve, another
    // variant's do not — on the occurrence and the relation side alike.
    shard.set_live({variants[2]});
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, 3 * 16 + 1) == 1003u);
    ZASSERT(hash_at(shard, 5 * 16 + 1) == 0u);
    ZASSERT(reference_count(shard, 999) == std::size_t(2));
}

ZEST_CASE(MaskTier32) {
    expect_tier_behavior(5);
}

ZEST_CASE(MaskTier64) {
    expect_tier_behavior(40);
}

ZEST_CASE(MaskTierRoaring) {
    expect_tier_behavior(70);
}

ZEST_CASE(LongTokenEscape) {
    auto rows = simple_rows({
        {{0, 300},   111},
        {{400, 404}, 222}
    });
    std::string content(500, 'y');
    auto shard = make_shard(write_fresh(rows, content));

    bool found = false;
    shard.lookup(299, [&](const index::Occurrence& o) {
        found = true;
        ZEXPECT(o.range.end == 300u);
        return false;
    });
    ZASSERT(found);
    ZASSERT(hash_at(shard, 402) == 222u);
}

ZEST_CASE(RelationPayloadRoundtrip) {
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
        ZEXPECT(extent.begin == 0u);
        ZEXPECT(extent.end == 50u);
        return false;
    });
    ZASSERT(checked_definition);

    bool checked_reference = false;
    shard.lookup(111, RelationKind::Reference, [&](const index::Relation& r) {
        checked_reference = true;
        ZEXPECT(r.target_symbol == 0u);
        return false;
    });
    ZASSERT(checked_reference);

    bool checked_pair = false;
    shard.lookup(333, RelationKind::Base, [&](const index::Relation& r) {
        checked_pair = true;
        ZEXPECT(r.target_symbol == 444u);
        return false;
    });
    ZASSERT(checked_pair);
}

ZEST_CASE(LocalSymbolNames) {
    build_index(R"(
        static int §(local)⟦§(local)helper⟧() { return 1; }
        int visible() { return §(use)⟦§(use)helper⟧(); }
    )");

    auto shard = make_shard(main_blob());

    auto local = hash_at(shard, point("use"));
    ZASSERT(local != 0);
    auto local_identity = shard.find_symbol(local);
    ZASSERT(local_identity);
    ZASSERT(local_identity->name == "helper");
    ZASSERT(index::has_flag(local_identity->flags, index::SymbolFlags::HasDefinition));
    ZASSERT(local_identity->parent == 0u);

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
    ZASSERT(external != 0);
    ZASSERT(!shard.find_symbol(external).has_value());
}

ZEST_CASE(MergedLocalFlagsUnion) {
    // Variants of one file can see different facts of a local symbol (a
    // definition behind `#ifdef`); the merged table keeps their union in
    // either merge order.
    llvm::StringRef content = "static int helper();\n";
    auto variant = [&](index::SymbolFlags flags) {
        auto rows = simple_rows({
            {{11, 17}, 7}
        });
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::write_shard(
            rows,
            [&](index::SymbolHash) -> std::optional<index::SymbolIdentity> {
                return index::SymbolIdentity{.name = "helper",
                                             .kind = SymbolKind::Function,
                                             .scope = index::SymbolScope::TULocal,
                                             .flags = flags};
            },
            content,
            os);
        return make_shard(bytes);
    };
    auto defining = variant(index::SymbolFlags::HasDefinition);
    auto declaring = variant(index::SymbolFlags::None);

    for(auto [first, second]: {
            std::pair{&defining,  &declaring},
            std::pair{&declaring, &defining }
    }) {
        std::vector<index::Shard> fresh;
        fresh.push_back(make_shard(second->bytes()));
        auto merged = merge(*first, first->variants(), std::move(fresh));
        auto identity = merged.find_symbol(7);
        ZASSERT(identity);
        ZASSERT(index::has_flag(identity->flags, index::SymbolFlags::HasDefinition));
    }
}

ZEST_CASE(MergedLocalNames) {
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
    ZASSERT(local != 0);
    auto local_identity = shard.find_symbol(local);
    ZASSERT(local_identity);
    ZASSERT(local_identity->name == "helper");
}

ZEST_CASE(WideSymbolIds) {
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
    ZASSERT(hash_at(shard, 69999 * 8 + 1) == 0x100000u + 69999);
    ZASSERT(hash_at(shard, 3 * 8 + 1) == 0x100000u + 3);
}

ZEST_CASE(UnloadedShardNoops) {
    index::Shard shard;
    shard.lookup(0, [&](const index::Occurrence&) { return true; });
    shard.lookup(1, RelationKind::Reference, [&](const index::Relation&) { return true; });
    ZASSERT(!shard.find_symbol(1).has_value());
    ZASSERT(shard.content().empty());
    ZASSERT(shard.line_starts().empty());
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
    auto starts = kota::ipc::lsp::line_starts(std::string_view(text.data(), text.size()));
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

ZEST_CASE(CorruptBlobRejected) {
    ZASSERT(!index::Shard::from_bytes("not a flatbuffer").loaded());

    // A valid blob cut mid-structure must fail verification, not be
    // misread. (One trailing byte can be alignment padding, so the cut
    // must reach real data.)
    auto rows = simple_rows({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(rows, "aaaa");
    ZASSERT(
        !index::Shard::from_bytes(llvm::StringRef(bytes).take_front(bytes.size() / 2)).loaded());

    // A structurally valid table of the current version with no line table
    // at all cannot be writer output.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ZASSERT(stale);
    auto data = llvm::StringRef(reinterpret_cast<const char*>(stale->data()), stale->size());
    ZASSERT(!index::Shard::from_bytes(data).loaded());
}

ZEST_CASE(ReservedLocalParentRejected) {
    // A local symbol's parent becomes a DenseSet key in a query's
    // container walk, so a sentinel value marks a corrupt blob.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaåå");
    blob.variants = {1};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    blob.local_syms = {0};
    blob.local_names = {"helper"};
    blob.local_kinds = {0};
    blob.local_scopes = {1};
    blob.local_args = {""};
    blob.local_parents = {0};
    blob.local_flags = {0};

    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.local_parents = {~std::uint64_t(0)};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(ContentHashMismatchRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    // Same byte length (ä is two UTF-8 bytes like å): only the hash
    // differs, so the size check cannot be what rejects the blob.
    blob.content = "aaåä";
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(StoredAsciiContentRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.content = "aaaa";
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(LineTableMismatchRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.line_lengths = {4, 3};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.line_lengths = {};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // Redistributing bytes between lines preserves the sum; with stored
    // content every line start must match the one the content derives.
    fill_content(blob, "aå\nbb\n");
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.line_lengths = {3, 4};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(MisorderedRowsRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    // Begins out of order.
    blob.occs.packed = {index::pack_range(8, index::length_escape), index::pack_range(0, 3)};
    blob.occs.long_rows = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // Begins sorted, but the escaped end regresses below the row before.
    blob.occs.packed = {index::pack_range(0, index::length_escape), index::pack_range(8, 3)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {14};  // ends decode to {14, 11}
    ZASSERT(!make_shard(bytes_of()).loaded());

    // An escaped end before its own begin.
    blob.occs.packed = {index::pack_range(0, 3), index::pack_range(8, index::length_escape)};
    blob.occs.long_rows = {1};
    blob.occs.long_ends = {5};  // row 1: begin 8, end 5
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(DuplicateOccKeyRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occ_syms8 = {0, 0};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.occ_syms8 = {1, 0};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(UnsortedRelationRowsRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.rels.packed = {index::pack_range(4, 3), index::pack_range(0, 3)};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.rels.packed = {index::pack_range(0, 3), index::pack_range(0, 3)};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(EscapeTableMismatchRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    // A sentinel without its sparse entry.
    blob.occs.long_rows = {};
    blob.occs.long_ends = {};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // A sparse entry pointing at an unescaped row.
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {260};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The relation escape table is validated alike.
    blob.occs.packed = {index::pack_range(0, index::length_escape)};
    blob.sym_rel_offsets = {0, 1};
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.rels.packed = {index::pack_range(0, index::length_escape)};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(RangesBeyondContentRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    // A plain length overruns the 16-byte content.
    blob.occs.packed = {index::pack_range(0, 100)};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // An escaped end does too.
    blob.occs.packed = {index::pack_range(0, index::length_escape)};
    blob.occs.long_rows = {0};
    blob.occs.long_ends = {600};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.occs.packed = {index::pack_range(0, 3)};
    blob.occs.long_rows = {};
    blob.occs.long_ends = {};

    // Relation ranges are bounded alike.
    blob.rels.packed = {index::pack_range(0, 100)};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // Except the no-range sentinel a pair relation legitimately carries —
    // on a source-located kind the same sentinel is corruption.
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    blob.rels.packed = {index::packed_sentinel};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.rels.packed = {index::pack_range(0, 3)};

    // And definition-range payloads.
    blob.rel_def_rows = {0};
    blob.rel_def_begins = {0};
    blob.rel_def_ends = {600};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(WrongRangeTierRejected) {
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
    ZASSERT(!make_shard(bytes).loaded());
}

ZEST_CASE(WrongSymWidthRejected) {
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
    ZASSERT(!make_shard(bytes).loaded());
}

ZEST_CASE(DuplicateSymbolHashRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.sym_hashes = {111, 111};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(DuplicateVariantRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.variants = {1, 1};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(StraySymbolIdRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occ_syms8 = {5};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.occ_syms8 = {0};

    blob.rel_sym8 = {5};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(MismatchedPayloadTableRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.rel_def_rows = {};
    blob.rel_def_begins = {};
    blob.rel_def_ends = {};
    blob.rel_sym_rows = {0};
    blob.rel_sym8 = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.rel_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.rel_sym_rows = {};
    blob.rel_sym8 = {};
    blob.rel_def_rows = {0};
    blob.rel_def_begins = {0};
    blob.rel_def_ends = {8};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(OwnerlessMaskRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    // An empty mask, then one whose only bit lies past the variant table.
    blob.occs.masks32 = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.occs.masks32 = {0b100};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The u64 tier is bounded alike.
    for(std::uint32_t i = 3; i <= 40; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occs.masks32 = {};
    blob.occs.masks64 = {1};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.occs.masks64 = {std::uint64_t(1) << 45};
    ZASSERT(!make_shard(bytes_of()).loaded());

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
    ZASSERT(make_shard(bytes_of()).loaded());
    set_mask({});
    ZASSERT(!make_shard(bytes_of()).loaded());
    clice::Bitmap stray;
    stray.add(70);
    set_mask(stray);
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(CorruptRoaringMaskRejected) {
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
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occs.roaring = {0xff, 0xff, 0xff};
    blob.occs.roaring_offsets = {0, 3};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The payload offset in the image's header, which the in-place view
    // follows, pointing past the slice.
    blob.occs.roaring.clear();
    for(auto byte: index::write_bitmap(mask)) {
        blob.occs.roaring.push_back(static_cast<std::uint8_t>(byte));
    }
    blob.occs.roaring[12] = 0xff;
    blob.occs.roaring_offsets = {0, static_cast<std::uint32_t>(blob.occs.roaring.size())};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(RebindSwapsIdenticalBytes) {
    build_index("int rebind_value() { return 1; }\n");
    auto bytes = main_blob();
    ZASSERT(!bytes.empty());
    auto shard = make_shard(bytes);
    ZASSERT(shard.loaded());
    auto hash_before = shard.content_hash();
    const char* address_before = shard.bytes().data();

    ZASSERT(shard.rebind(llvm::MemoryBuffer::getMemBufferCopy(bytes)));
    ZASSERT(shard.bytes().data() != address_before);
    ZASSERT(shard.content_hash() == hash_before);

    // Byte identity is the caller's contract: only the size gates the
    // swap, so migration never touches the replacement's content pages.
    std::string drift = bytes;
    drift.back() = static_cast<char>(drift.back() ^ 1);
    ZASSERT(shard.rebind(llvm::MemoryBuffer::getMemBufferCopy(drift)));

    // A missing replacement or another size is rejected; the current
    // buffer stays.
    const char* kept = shard.bytes().data();
    ZASSERT(!shard.rebind(llvm::MemoryBuffer::getMemBufferCopy(bytes + "x")));
    ZASSERT(!shard.rebind(nullptr));
    ZASSERT(shard.bytes().data() == kept);
}

};  // ZEST_SUITE(Shard)

}  // namespace
}  // namespace clice::testing
