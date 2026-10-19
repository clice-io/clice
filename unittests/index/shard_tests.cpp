module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.indexer;
import :index.shard;
import :index.tu_index;
import :support.archive;
import :syntax.position;
import :unittests.utils.tester;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(Shard, Tester) {

index::TUIndex tu_index;

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_focus("main.cpp", code);
    ZASSERT(compile());
    tu_index = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
    ZASSERT(tu_index.loaded());
}

std::string write_fresh(const index::FileIndex& file_index, llvm::StringRef content) {
    return index::write_shard(file_index, {}, content);
}

/// The focus file's worker-encoded blob, straight from the TU index.
std::string focus_blob() {
    return tu_index.shard_of(tu_index.path_count() - 1).bytes().str();
}

/// Owning wrap: from_bytes borrows, and every builder here returns a
/// temporary string.
index::Shard make_shard(llvm::StringRef bytes) {
    return index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
}

index::Shard merge(const index::Shard& old,
                   llvm::ArrayRef<index::VariantHash> keep,
                   std::vector<index::Shard> fresh) {
    auto bytes = index::merge_shards(old, keep, fresh);
    return make_shard(bytes);
}

/// `content` must repeat the text the shard was built from — ASCII blobs
/// do not store it, so it cannot be recovered from `old`.
index::Shard append_variant(const index::Shard& old,
                            const index::FileIndex& file_index,
                            llvm::StringRef content) {
    std::vector<index::Shard> fresh;
    fresh.push_back(make_shard(write_fresh(file_index, content)));
    return merge(old, old.variants(), std::move(fresh));
}

index::SymbolHash hash_at(const index::Shard& shard, std::uint32_t offset) {
    index::SymbolHash result = 0;
    shard.occurrences_at(offset, [&](const index::Occurrence& o) {
        result = o.target;
        return false;
    });
    return result;
}

index::FileIndex simple_index(std::initializer_list<index::Occurrence> occurrences) {
    index::FileIndex file_index;
    file_index.occurrences = occurrences;
    return file_index;
}

ZEST_CASE(RoundtripLookups) {
    build_index(R"(
        int §(def)⟦§(def)foo⟧() { return 42; }
        int bar() { return §(ref)⟦§(ref)foo⟧(); }
    )");

    auto content = sources.all_files.find("main.cpp")->second.content;
    auto shard = make_shard(focus_blob());
    ZASSERT(shard.loaded());
    ZASSERT(shard.content_size() == static_cast<std::uint32_t>(content.size()));
    ZASSERT(!shard.line_starts().empty());

    auto expected = range("ref");
    bool found = false;
    shard.occurrences_at(point("ref"), [&](const index::Occurrence& o) {
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
    shard.relations_of(symbol, RelationKind::Definition, [&](const index::Relation& r) {
        has_definition = true;
        ZEXPECT(r.range.begin == range("def").begin);
        return false;
    });
    ZASSERT(has_definition);
}

ZEST_CASE(DeterministicEncoding) {
    // The blob's byte hash is the variant's identity, so equal entries must
    // encode to equal bytes regardless of in-memory insertion order.
    index::FileIndex file_index;
    file_index.occurrences = {
        {{0, 3},   111},
        {{10, 13}, 222},
        {{20, 23}, 111},
    };
    file_index.relations[111] = {
        {.kind = RelationKind::Definition, .range = {0, 3},   .target = 0},
        {.kind = RelationKind::Reference,  .range = {20, 23}, .target = 0},
    };
    file_index.relations[333] = {
        {.kind = RelationKind::Base, .range = {10, 13}, .target = 444},
    };

    index::FileIndex shuffled;
    shuffled.occurrences = {file_index.occurrences[2],
                            file_index.occurrences[0],
                            file_index.occurrences[1]};
    shuffled.relations[333] = file_index.relations[333];
    shuffled.relations[111] = {file_index.relations[111][1], file_index.relations[111][0]};

    auto content = "aaa bbb ccc ddd 111 222 333";
    ZASSERT(write_fresh(file_index, content) == write_fresh(shuffled, content));
}

ZEST_CASE(AnonymousVariantIdentity) {
    auto file_index = simple_index({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(file_index, "aaa bbb");
    auto shard = make_shard(bytes);

    auto variants = shard.variants();
    ZASSERT(variants.size() == std::size_t(1));
    ZASSERT(variants.front() == llvm::xxh3_64bits(bytes));
    ZASSERT(shard.has_variant(variants.front()));
}

ZEST_CASE(ContentNotStored) {
    std::string content = "int x;\nint y;\n";
    auto file_index = simple_index({
        {{4, 5}, 111}
    });
    auto shard = make_shard(write_fresh(file_index, content));

    ZASSERT(shard.positions().text().empty());
    ZASSERT(shard.content_size() == static_cast<std::uint32_t>(content.size()));
    ZASSERT(shard.content_hash() == llvm::xxh3_64bits(content));
    ZASSERT(hash_at(shard, 4) == 111u);

    auto expected = kota::ipc::lsp::line_starts(content);
    auto starts = shard.line_starts();
    ZASSERT(std::vector<std::uint32_t>(starts.begin(), starts.end()) == expected);
}

ZEST_CASE(MisalignedBytesLoad) {
    // A blob embedded in another (a TU index's sections) sits at whatever
    // offset the outer layout gave it, while its columns are read in place.
    auto file_index = simple_index({
        {{4, 5}, 111}
    });
    auto bytes = write_fresh(file_index, "int x;\n");
    std::vector<std::uint64_t> storage(bytes.size() / sizeof(std::uint64_t) + 2);
    auto* misaligned = reinterpret_cast<char*>(storage.data()) + sizeof(std::uint32_t);
    std::memcpy(misaligned, bytes.data(), bytes.size());

    auto shard = index::Shard::from_bytes(llvm::StringRef(misaligned, bytes.size()));
    ZASSERT(shard.loaded());
    ZASSERT(hash_at(shard, 4) == 111u);
}

ZEST_CASE(CRLFLinesMarked) {
    // Content is not stored: the shard marks the lines ending in "\r\n" so
    // its coordinates place their ends without the text.
    std::string content = "int x;\r\nint y;\nint z;\r\n";
    auto file_index = simple_index({
        {{4, 5}, 111}
    });
    auto shard = make_shard(write_fresh(file_index, content));
    auto more = simple_index({
        {{12, 13}, 222}
    });
    auto merged = append_variant(shard, more, content);

    auto starts = shard.line_starts();
    PositionMap scanned(content, starts);
    for(std::uint32_t line = 0; line < starts.size(); line += 1) {
        ZEXPECT(shard.positions().line_bounds(line) == scanned.line_bounds(line));
        ZEXPECT(merged.positions().line_bounds(line) == scanned.line_bounds(line));
    }
}

ZEST_CASE(NonAsciiLinesMapPositions) {
    // Only the lines holding non-ASCII bytes keep their text, which is all
    // UTF-16 and UTF-32 mapping reads: every conversion agrees with the map
    // over the whole text — inside and past code points, in line ends, on
    // ASCII and non-ASCII lines, past the last line.
    std::string content =
        "int å;\r\nint y; // \xF0\x9F\x98\x80 ü\n\n// " + std::string(300, 'a') + "é\nx = \"é\";";
    auto file_index = simple_index({
        {{4, 6}, 111}
    });
    auto shard = make_shard(write_fresh(file_index, content));
    auto starts = kota::ipc::lsp::line_starts(content);
    PositionMap text(content, starts);
    auto stored = shard.positions();
    using Position = kota::ipc::protocol::Position;
    auto point = [](const std::optional<Position>& position) {
        return position ? std::optional(std::pair(position->line, position->character))
                        : std::nullopt;
    };
    auto span = [&](const std::optional<kota::ipc::protocol::Range>& range) {
        return range ? std::optional(std::pair(point(range->start), point(range->end)))
                     : std::nullopt;
    };
    for(auto encoding: {PositionEncoding::UTF8, PositionEncoding::UTF16, PositionEncoding::UTF32}) {
        for(std::uint32_t offset = 0; offset <= content.size(); offset += 1) {
            ZEXPECT(point(stored.position(offset, encoding)) ==
                    point(text.position(offset, encoding)));
            ZEXPECT(span(stored.range({0, offset}, encoding)) ==
                    span(text.range({0, offset}, encoding)));
        }
        for(std::uint32_t line = 0; line <= starts.size(); line += 1) {
            for(std::uint32_t character = 0; character < 24; character += 1) {
                Position position{.line = line, .character = character};
                ZEXPECT(stored.offset(position, encoding) == text.offset(position, encoding));
                ZEXPECT(stored.offset_clamped(position, encoding) ==
                        text.offset_clamped(position, encoding));
                kota::ipc::protocol::Range range{
                    .start = {.line = 0, .character = 5},
                    .end = position
                };
                ZEXPECT(stored.offset_range(range, encoding) == text.offset_range(range, encoding));
            }
        }
    }
}

ZEST_CASE(LongLineEscape) {
    // A line past 255 bytes escapes to the sparse table; the materialized
    // starts must match a direct scan of the content.
    std::string content = "short\n" + std::string(300, 'a') + "\nshort again\n";
    auto file_index = simple_index({
        {{0, 5}, 111}
    });
    auto shard = make_shard(write_fresh(file_index, content));

    auto expected = kota::ipc::lsp::line_starts(content);
    auto starts = shard.line_starts();
    ZASSERT(std::vector<std::uint32_t>(starts.begin(), starts.end()) == expected);
}

ZEST_CASE(WideRangeEncoding) {
    // Past 16MB of content the packed range column cannot hold begins;
    // the wide encoding takes over transparently.
    std::string content(index::packed_range_limit + 64, 'w');
    auto file_index = simple_index({
        {{0, 3},                                                          111},
        {{index::packed_range_limit + 8, index::packed_range_limit + 11}, 222},
    });
    auto shard = make_shard(write_fresh(file_index, content));
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, index::packed_range_limit + 9) == 222u);
}

ZEST_CASE(VariantMaskFiltering) {
    auto a = simple_index({
        {{0, 3}, 111}
    });
    auto b = simple_index({
        {{0, 3},   111},
        {{10, 13}, 222}
    });

    auto first = make_shard(write_fresh(a, "aaa bbb ccc ddd"));
    auto shard = append_variant(first, b, "aaa bbb ccc ddd");
    auto variants = shard.variants();
    ZASSERT(variants.size() == std::size_t(2));

    // All variants live by default: both occurrences serve.
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, 11) == 222u);

    // Restricting to the first variant hides the occurrence only the second
    // holds, while the shared occurrence keeps serving.
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
    auto a = simple_index({
        {{0, 3}, 111}
    });
    auto b = simple_index({
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
    // Several fresh variants are merged in one write; shared occurrences collapse
    // with OR-ed masks and each unique occurrence stays filterable to its owner.
    std::string content = "aaa bbb ccc ddd eee";
    std::vector<index::Shard> fresh;
    for(std::uint32_t i = 0; i < 3; i += 1) {
        auto file_index = simple_index({
            {{0, 3},                         111     },
            {{4 * (i + 1), 4 * (i + 1) + 3}, 1000 + i},
        });
        fresh.push_back(make_shard(write_fresh(file_index, content)));
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
        auto file_index = simple_index({
            {{0, 3},               111     },
            {{i * 16, i * 16 + 3}, 1000 + i}
        });
        file_index.relations[999] = {
            {.kind = RelationKind::Reference, .range = {0, 3},               .target = 0},
            {.kind = RelationKind::Reference, .range = {i * 16, i * 16 + 3}, .target = 0},
        };
        auto fresh = make_shard(write_fresh(file_index, content));
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
    shard.relations_of(symbol, RelationKind::Reference, [&](const index::Relation&) {
        count += 1;
        return true;
    });
    return count;
}

void expect_encoding_behavior(std::uint32_t count) {
    auto shard = grow_variants(count);
    auto variants = shard.variants();
    ZASSERT(variants.size() == std::size_t(count));

    // Every variant's unique occurrence serves under the full live set, and the
    // shared relation collapsed to one entry across all variants.
    for(std::uint32_t i = 1; i <= count; i += 1) {
        ZASSERT(hash_at(shard, i * 16 + 1) == 1000u + i);
    }
    ZASSERT(reference_count(shard, 999) == std::size_t(count) + 1);

    // One live variant: its unique entries and the shared entries serve, another
    // variant's do not — on the occurrence and the relation side alike.
    shard.set_live({variants[2]});
    ZASSERT(hash_at(shard, 1) == 111u);
    ZASSERT(hash_at(shard, 3 * 16 + 1) == 1003u);
    ZASSERT(hash_at(shard, 5 * 16 + 1) == 0u);
    ZASSERT(reference_count(shard, 999) == std::size_t(2));
}

ZEST_CASE(MaskEncoding8) {
    expect_encoding_behavior(5);
}

ZEST_CASE(MaskEncoding16) {
    expect_encoding_behavior(12);
}

ZEST_CASE(MaskEncoding32) {
    expect_encoding_behavior(20);
}

ZEST_CASE(MaskEncoding64) {
    expect_encoding_behavior(40);
}

ZEST_CASE(MaskEncodingRoaring) {
    expect_encoding_behavior(70);
}

ZEST_CASE(LongTokenEscape) {
    auto file_index = simple_index({
        {{0, 300},   111},
        {{400, 404}, 222}
    });
    std::string content(500, 'y');
    auto shard = make_shard(write_fresh(file_index, content));

    bool found = false;
    shard.occurrences_at(299, [&](const index::Occurrence& o) {
        found = true;
        ZEXPECT(o.range.end == 300u);
        return false;
    });
    ZASSERT(found);
    ZASSERT(hash_at(shard, 402) == 222u);
}

ZEST_CASE(RelationPayloadRoundtrip) {
    index::FileIndex file_index;
    index::Relation definition{
        .kind = RelationKind::Definition,
        .range = {0, 3}
    };
    definition.set_extent({0, 50});
    file_index.relations[111] = {
        definition,
        {.kind = RelationKind::Reference, .range = {10, 13}, .target = 0},
    };
    file_index.relations[333] = {
        {.kind = RelationKind::Base, .range = {20, 23}, .target = 444},
    };

    auto shard = make_shard(write_fresh(file_index, std::string(60, 'z')));

    bool checked_definition = false;
    shard.relations_of(111, RelationKind::Definition, [&](const index::Relation& r) {
        checked_definition = true;
        auto extent = index::Relation(r).extent();
        ZEXPECT(extent.begin == 0u);
        ZEXPECT(extent.end == 50u);
        return false;
    });
    ZASSERT(checked_definition);

    bool checked_reference = false;
    shard.relations_of(111, RelationKind::Reference, [&](const index::Relation& r) {
        checked_reference = true;
        ZEXPECT(r.target == 0u);
        return false;
    });
    ZASSERT(checked_reference);

    bool checked_pair = false;
    shard.relations_of(333, RelationKind::Base, [&](const index::Relation& r) {
        checked_pair = true;
        ZEXPECT(r.target == 444u);
        return false;
    });
    ZASSERT(checked_pair);
}

ZEST_CASE(PayloadsAcrossBlocks) {
    // Readers place a payload by its block's counts: enough relations for
    // several blocks, the target and extent payloads in different blocks,
    // and extents that do not fit the u16 columns beside ones that do.
    index::FileIndex file_index;
    constexpr std::uint32_t count = 100;
    auto extent_of = [](std::uint32_t i) -> LocalSourceRange {
        auto begin = i * 100;
        switch(i % 4) {
            case 0: return {begin, begin + 0x10001};
            case 1: return {begin + 1, begin + 10};
            default: return {begin - i % 4, begin + 40};
        }
    };
    for(std::uint32_t i = 0; i < count; i += 1) {
        file_index.relations[50].push_back({
            .kind = RelationKind::Base,
            .range = {i * 100 + 70, i * 100 + 73},
            .target = 500 + i
        });
        index::Relation definition{
            .kind = RelationKind::Definition,
            .range = {i * 100, i * 100 + 3}
        };
        definition.set_extent(extent_of(i));
        file_index.relations[111].push_back(definition);
        file_index.relations[111].push_back({
            .kind = RelationKind::Reference,
            .range = {i * 100 + 50, i * 100 + 53},
            .target = 0
        });
        file_index.relations[333].push_back({
            .kind = RelationKind::Base,
            .range = {i * 100 + 60, i * 100 + 63},
            .target = 1000 + i
        });
    }
    auto shard = make_shard(write_fresh(file_index, std::string(count * 100 + 0x10010, 'z')));
    ZASSERT(shard.loaded());

    std::uint32_t seen = 0;
    shard.relations_of(111, RelationKind::Definition, [&](const index::Relation& r) {
        auto i = r.range.begin / 100;
        auto extent = index::Relation(r).extent();
        ZEXPECT(extent.begin == extent_of(i).begin);
        ZEXPECT(extent.end == extent_of(i).end);
        seen += 1;
        return true;
    });
    ZEXPECT(seen == count);
    for(auto [symbol, first]: {
            std::pair{50u,  500u },
            std::pair{333u, 1000u}
    }) {
        seen = 0;
        shard.relations_of(symbol, RelationKind::Base, [&](const index::Relation& r) {
            ZEXPECT(r.target == first + r.range.begin / 100);
            seen += 1;
            return true;
        });
        ZEXPECT(seen == count);
    }
}

ZEST_CASE(MergedRelationPayloads) {
    // Every relation group keeps its own payloads through a merge, in the
    // stored blob and in the fresh one alike.
    index::FileIndex first;
    index::Relation definition{
        .kind = RelationKind::Definition,
        .range = {0, 3}
    };
    definition.set_extent({0, 50});
    first.relations[111] = {
        definition,
        {.kind = RelationKind::Reference, .range = {10, 13}, .target = 0},
    };
    first.relations[333] = {
        {.kind = RelationKind::Base, .range = {20, 23}, .target = 444},
    };
    first.relations[555] = {
        {.kind = RelationKind::Base, .range = {30, 33}, .target = 666},
    };
    auto second = first;
    second.relations[333].push_back({
        .kind = RelationKind::Base,
        .range = {40, 43},
        .target = 777
    });

    using Entry =
        std::tuple<index::SymbolHash, std::uint8_t, std::uint32_t, std::uint32_t, std::uint64_t>;
    auto relations_of = [](const index::Shard& shard) {
        std::vector<Entry> relations;
        shard.for_each_relation([&](index::SymbolHash symbol, const index::Relation& relation) {
            relations.emplace_back(symbol,
                                   static_cast<std::uint8_t>(relation.kind),
                                   relation.range.begin,
                                   relation.range.end,
                                   relation.target);
            return true;
        });
        return relations;
    };
    std::string content(60, 'z');
    auto expected = relations_of(make_shard(write_fresh(second, content)));
    ZASSERT(expected.size() == std::size_t(5));
    auto merged = append_variant(make_shard(write_fresh(first, content)), second, content);
    ZASSERT(merged.variants().size() == std::size_t(2));
    ZEXPECT(relations_of(merged) == expected);
}

ZEST_CASE(MergedNonAsciiLines) {
    // A merged blob carries the non-ASCII lines its variants were built
    // with: without them it would still load, mapping their UTF-16
    // positions as bytes.
    std::string content = "int a;\nint é = 1; // ü\nint b;\n";
    auto shard = make_shard(write_fresh(simple_index({
                                            {{4, 5}, 111}
    }),
                                        content));
    shard = append_variant(shard,
                           simple_index({
                               {{4, 5}, 222}
    }),
                           content);
    ZASSERT(shard.variants().size() == std::size_t(2));
    auto starts = kota::ipc::lsp::line_starts(content);
    PositionMap text(content, starts);
    auto stored = shard.positions();
    for(std::uint32_t offset = 0; offset <= content.size(); offset += 1) {
        auto expected = text.position(offset, PositionEncoding::UTF16);
        auto actual = stored.position(offset, PositionEncoding::UTF16);
        ZEXPECT(actual.has_value() == expected.has_value());
        if(actual && expected) {
            ZEXPECT(actual->line == expected->line);
            ZEXPECT(actual->character == expected->character);
        }
    }
}

ZEST_CASE(LocalSymbolNames) {
    build_index(R"(
        static int §(local)⟦§(local)helper⟧() { return 1; }
        int visible() { return §(use)⟦§(use)helper⟧(); }
    )");

    auto shard = make_shard(focus_blob());

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
        tu_index.for_each_symbol([&](index::SymbolHash hash,
                                     const index::SymbolView& symbol,
                                     llvm::ArrayRef<std::uint32_t>) {
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

ZEST_CASE(LocalParentsAndSignatures) {
    // A local's parent is an id into the blob's own symbol table, so the
    // parent joins the table even where no entry names it; signatures and
    // arguments are stored for the few locals having them.
    build_index(R"(
        static int §(fn)⟦§(fn)twice⟧(int §(param)⟦§(param)value⟧) {
            return §(use)⟦§(use)value⟧ * 2;
        }
        int visible() { return twice(1); }
    )");

    auto shard = make_shard(focus_blob());
    auto function = hash_at(shard, point("fn"));
    auto parameter = hash_at(shard, point("use"));
    ZASSERT(function != 0);
    ZASSERT(parameter != 0);
    auto function_identity = shard.find_symbol(function);
    ZASSERT(function_identity);
    ZASSERT(function_identity->name == "twice");
    ZASSERT(!function_identity->signature.empty());
    auto parameter_identity = shard.find_symbol(parameter);
    ZASSERT(parameter_identity);
    ZASSERT(parameter_identity->name == "value");
    ZASSERT(parameter_identity->parent == function);
    ZASSERT(parameter_identity->signature.empty());
    ZASSERT(parameter_identity->args.empty());
}

ZEST_CASE(MergedLocalFlagsUnion) {
    // Variants of one file can see different facts of a local symbol (a
    // definition behind `#ifdef`, `struct` declaring what `class` defines);
    // the merged table keeps their union and the definition's token kind in
    // either merge order.
    llvm::StringRef content = "static int helper();\n";
    auto variant = [&](index::SymbolFlags flags, SemanticTokenKind token_kind) {
        auto file_index = simple_index({
            {{11, 17}, 7}
        });
        auto bytes = index::write_shard(
            file_index,
            [&](index::SymbolHash) -> std::optional<index::SymbolView> {
                return index::SymbolView{.name = "helper",
                                         .kind = SymbolKind::Function,
                                         .scope = index::SymbolScope::Internal,
                                         .flags = flags,
                                         .token_class = {.kind = token_kind}};
            },
            content);
        return make_shard(bytes);
    };
    auto defining = variant(index::SymbolFlags::HasDefinition, SemanticTokenKind::Class);
    auto declaring = variant(index::SymbolFlags::None, SemanticTokenKind::Struct);

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
        ZASSERT(identity->token_class.kind == SemanticTokenKind::Class);
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
    auto first = make_shard(focus_blob());

    auto extra = simple_index({
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
    index::FileIndex file_index;
    constexpr std::uint32_t count = 70000;
    file_index.occurrences.reserve(count);
    for(std::uint32_t i = 0; i < count; i += 1) {
        file_index.occurrences.push_back({
            {i * 8, i * 8 + 3},
            0x100000u + i
        });
    }
    std::string content(count * 8 + 16, 'w');
    auto shard = make_shard(write_fresh(file_index, content));
    ZASSERT(hash_at(shard, 69999 * 8 + 1) == 0x100000u + 69999);
    ZASSERT(hash_at(shard, 3 * 8 + 1) == 0x100000u + 3);
}

ZEST_CASE(UnloadedShardNoops) {
    index::Shard shard;
    shard.occurrences_at(0, [&](const index::Occurrence&) { return true; });
    shard.relations_of(1, RelationKind::Reference, [&](const index::Relation&) { return true; });
    ZASSERT(!shard.find_symbol(1).has_value());
    ZASSERT(shard.line_starts().empty());
}

/// Fill the content identity, line table and non-ASCII lines of a hand-built
/// blob the way the writer would.
void fill_content(index::ShardBlob& blob, llvm::StringRef text) {
    blob.content_hash = llvm::xxh3_64bits(text);
    blob.content_size = static_cast<std::uint32_t>(text.size());
    blob.line_lengths.clear();
    blob.long_lines.clear();
    blob.long_line_lengths.clear();
    blob.utf8_lines.clear();
    blob.utf8_text.clear();
    blob.utf8_text_ends.clear();
    auto starts = kota::ipc::lsp::line_starts(std::string_view(text.data(), text.size()));
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto last = i + 1 == starts.size();
        auto next = last ? blob.content_size : starts[i + 1];
        auto length = next - starts[i];
        auto line = text.slice(starts[i], last ? next : next - 1);
        if(llvm::any_of(line, [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
            blob.utf8_lines.push_back(static_cast<std::uint32_t>(i));
            blob.utf8_text += line;
            blob.utf8_text_ends.push_back(static_cast<std::uint32_t>(blob.utf8_text.size()));
        }
        if(length >= index::length_escape) {
            blob.line_lengths.push_back(index::length_escape);
            blob.long_lines.push_back(static_cast<std::uint32_t>(i));
            blob.long_line_lengths.push_back(length);
        } else {
            blob.line_lengths.push_back(static_cast<std::uint8_t>(length));
        }
    }
}

/// Occurrence ranges in the delta encoding as the writer lays it out —
/// (begin, stored length) pairs — with a begin before its predecessor's
/// escaped the way no writer does.
void set_occurrences(index::ShardBlob& blob,
                     std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> entries) {
    auto& occurrences = blob.occurrences;
    occurrences.block_begins.clear();
    occurrences.deltas.clear();
    occurrences.far_entries.clear();
    occurrences.far_begins.clear();
    occurrences.lengths.clear();
    std::uint32_t previous = 0;
    std::uint32_t entry = 0;
    for(auto [begin, length]: entries) {
        if(entry % index::delta_block == 0) {
            occurrences.block_begins.push_back(begin);
            occurrences.deltas.push_back(0);
        } else if(begin < previous || begin - previous >= 255) {
            occurrences.deltas.push_back(255);
            occurrences.far_entries.push_back(entry);
            occurrences.far_begins.push_back(begin);
        } else {
            occurrences.deltas.push_back(static_cast<std::uint8_t>(begin - previous));
        }
        occurrences.lengths.push_back(length);
        previous = begin;
        entry += 1;
    }
}

ZEST_CASE(CorruptBlobRejected) {
    ZASSERT(!index::Shard::from_bytes("not an archive").loaded());

    // A valid blob cut short must fail verification, not be misread.
    auto file_index = simple_index({
        {{0, 3}, 111}
    });
    auto bytes = write_fresh(file_index, "aaaa");
    ZASSERT(
        !index::Shard::from_bytes(llvm::StringRef(bytes).take_front(bytes.size() / 2)).loaded());
}

ZEST_CASE(ReservedLocalParentRejected) {
    // A local symbol's parent becomes a DenseSet key in a query's
    // container walk, so a sentinel value marks a corrupt blob.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaåå");
    blob.variants = {1};
    blob.symbol_hashes = {111, ~std::uint64_t(0)};
    blob.relation_offsets = {0, 0, 0};
    blob.local_symbols = {0};
    blob.local_names = "helper";
    blob.local_name_ends = {6};
    blob.local_kinds = {0};
    blob.local_scopes = {1};
    blob.local_parents = {0};
    blob.local_flags = {0};
    blob.local_token_kinds = {0};
    blob.local_token_modifiers = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.local_parents = {2};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // So does a token kind past the enum: it indexes the legend's tables.
    blob.local_parents = {0};
    blob.local_token_kinds = {200};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.local_token_kinds = {0};

    // An argument list stored empty is the absent entry's to spell.
    blob.local_args_of = {0};
    blob.local_args_ends = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.local_args = "<int>";
    blob.local_args_ends = {5};
    auto shard = make_shard(bytes_of());
    ZASSERT(shard.loaded());
    auto helper = shard.find_symbol(111);
    ZASSERT(helper.has_value());
    ZASSERT(helper->name == "helper");
    ZASSERT(helper->args == "<int>");
    ZASSERT(helper->signature.empty());
    ZASSERT(helper->parent == 0);
}

ZEST_CASE(NonAsciiLineMismatchRejected) {
    // Position mapping reads a listed line's text in place of the line, so
    // it must be the line's text exactly; a pure-ASCII one is a second
    // spelling of the unlisted line.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aa\nåå\nbb");
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.utf8_lines = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.utf8_lines = {3};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.utf8_lines = {1};

    blob.utf8_text = "åå\n";
    blob.utf8_text_ends = {5};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.utf8_lines = {1, 2};
    blob.utf8_text = "ååbb";
    blob.utf8_text_ends = {4, 6};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(LineTableMismatchRejected) {
    // Line starts are prefix sums of the length column; a sum drifting off
    // the content size would shift every position mapping below the drift.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaa\nbbb\n");
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.line_lengths = {4, 3};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.line_lengths = {};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // Empty content is one empty line: no line table at all sums to its size
    // but cannot be writer output.
    fill_content(blob, "");
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.line_lengths = {};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(MisorderedEntriesRejected) {
    // Occurrence lookup binary-searches decoded entry ends; a corrupt blob
    // whose entries lost their order must load as "not on disk" and be
    // rebuilt, not keep misresolving queries on every restart.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, std::string(320, 'a'));
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    set_occurrences(blob,
                    {
                        {0, 3                   },
                        {8, index::length_escape}
    });
    blob.occurrences.long_entries = {1};
    blob.occurrences.long_ends = {263};
    blob.occurrence_symbols = {0, 0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    // Begins out of order, in a block and across blocks.
    set_occurrences(blob,
                    {
                        {8, index::length_escape},
                        {0, 3                   }
    });
    blob.occurrences.long_entries = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
    set_occurrences(blob,
                    {
                        {0,  1},
                        {1,  1},
                        {2,  1},
                        {3,  1},
                        {4,  1},
                        {5,  1},
                        {6,  1},
                        {7,  1},
                        {8,  1},
                        {9,  1},
                        {10, 1},
                        {11, 1},
                        {12, 1},
                        {13, 1},
                        {14, 1},
                        {15, 1},
                        {15, 1}
    });
    blob.occurrences.long_entries = {};
    blob.occurrences.long_ends = {};
    blob.occurrence_symbols.assign(17, 0);
    blob.symbol_hashes = {111, 222};
    blob.relation_offsets = {0, 0, 0};
    blob.occurrence_symbols.back() = 1;
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.occurrences.block_begins = {0, 14};
    ZASSERT(!make_shard(bytes_of()).loaded());
    // A block without its begin.
    blob.occurrences.block_begins = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    blob.occurrence_symbols = {0, 0};

    // A begin escaped though it moved less than the escape distance: a
    // second spelling of the same occurrences.
    set_occurrences(blob,
                    {
                        {0, 3                   },
                        {8, index::length_escape}
    });
    blob.occurrences.long_entries = {1};
    blob.occurrences.long_ends = {263};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.occurrences.deltas = {0, 255};
    blob.occurrences.far_entries = {1};
    blob.occurrences.far_begins = {8};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // Begins sorted, but the escaped end regresses below the entry before.
    set_occurrences(blob,
                    {
                        {0, index::length_escape},
                        {8, 3                   }
    });
    blob.occurrences.long_entries = {0};
    blob.occurrences.long_ends = {300};  // ends decode to {300, 11}
    ZASSERT(!make_shard(bytes_of()).loaded());

    // An escaped end before its own begin.
    set_occurrences(blob,
                    {
                        {0, 3                   },
                        {8, index::length_escape}
    });
    blob.occurrences.long_entries = {1};
    blob.occurrences.long_ends = {5};  // entry 1: begin 8, end 5
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(DuplicateOccKeyRejected) {
    // The merge two-way merges occurrence runs under the full (begin, end,
    // symbol) key and the writer combines equal keys, so a repeated or
    // descending symbol under one range is non-canonical and would
    // mis-merge silently instead of being rejected.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.symbol_hashes = {111, 222};
    blob.relation_offsets = {0, 0, 0};
    set_occurrences(blob,
                    {
                        {0, 3},
                        {0, 3}
    });
    blob.occurrence_symbols = {0, 1};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    // Positive control: one range with ascending symbols loads.
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occurrence_symbols = {0, 0};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.occurrence_symbols = {1, 0};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(UnsortedRelationsRejected) {
    // Entries of one relation group merge under the full (kind, begin, end,
    // payload) key and the writer sorts and combines equal keys, so
    // out-of-order or repeated entries are non-canonical and would mis-merge
    // silently instead of being rejected.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 2};
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Reference),
                           static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.relations.packed = {index::pack_range(0, 3), index::pack_range(4, 3)};
    blob.relation_target_ranks = {0};
    blob.relation_extent_ranks = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.relations.packed = {index::pack_range(4, 3), index::pack_range(0, 3)};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.relations.packed = {index::pack_range(0, 3), index::pack_range(0, 3)};
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
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    set_occurrences(blob,
                    {
                        {0, index::length_escape}
    });
    blob.occurrences.long_entries = {0};
    blob.occurrences.long_ends = {260};
    blob.occurrence_symbols = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    // A sentinel without its sparse entry.
    blob.occurrences.long_entries = {};
    blob.occurrences.long_ends = {};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // A sparse entry pointing at an unescaped occurrence.
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrences.long_entries = {0};
    blob.occurrences.long_ends = {260};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The relation escape table is validated alike.
    set_occurrences(blob,
                    {
                        {0, index::length_escape}
    });
    blob.relation_offsets = {0, 1};
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.relations.packed = {index::pack_range(0, index::length_escape)};
    blob.relation_target_ranks = {0};
    blob.relation_extent_ranks = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(ShortEscapedLengthRejected) {
    // A length escapes exactly when its byte cannot hold it: an escaped
    // shorter one is a second spelling of the same blob.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, std::string(300, 'a'));
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    set_occurrences(blob,
                    {
                        {0, index::length_escape}
    });
    blob.occurrences.long_entries = {0};
    blob.occurrences.long_ends = {255};
    blob.occurrence_symbols = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occurrences.long_ends = {254};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.occurrences.long_ends = {255};

    // The relation side alike.
    blob.relation_offsets = {0, 1};
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.relations.packed = {index::pack_range(0, index::length_escape)};
    blob.relations.long_entries = {0};
    blob.relations.long_ends = {255};
    blob.relation_target_ranks = {0};
    blob.relation_extent_ranks = {0};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.relations.long_ends = {254};
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
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 1};
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrence_symbols = {0};
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    blob.relations.packed = {index::pack_range(0, 3)};
    blob.relation_target_ranks = {0};
    blob.relation_extent_ranks = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    // A plain length overruns the 16-byte content.
    set_occurrences(blob,
                    {
                        {0, 100}
    });
    ZASSERT(!make_shard(bytes_of()).loaded());

    // An escaped end does too.
    set_occurrences(blob,
                    {
                        {0, index::length_escape}
    });
    blob.occurrences.long_entries = {0};
    blob.occurrences.long_ends = {600};
    ZASSERT(!make_shard(bytes_of()).loaded());
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrences.long_entries = {};
    blob.occurrences.long_ends = {};

    // Relation ranges are bounded alike.
    blob.relations.packed = {index::pack_range(0, 100)};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // Except the no-range sentinel a pair relation legitimately carries —
    // on a source-located kind the same sentinel is corruption.
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Base)};
    blob.relations.packed = {index::packed_sentinel};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Reference)};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.relations.packed = {index::pack_range(0, 3)};

    // And definition-range payloads.
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Definition) |
                           index::payload_bit};
    blob.relation_extent_heads = {0};
    blob.relation_extent_lengths = {600};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(WrongRangeEncodingRejected) {
    // The range encoding is a strict function of the side and the content
    // size — a second encoding of the same entries would fork the byte
    // identity.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    blob.occurrences.begins = {0};
    blob.occurrences.lengths = {3};
    blob.occurrence_symbols = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.occurrences = {};
    blob.occurrences.packed = {index::pack_range(0, 3)};
    ZASSERT(!make_shard(bytes_of()).loaded());

    blob.occurrences = {};
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    ZASSERT(make_shard(bytes_of()).loaded());
}

ZEST_CASE(WrongSymWidthRejected) {
    // The symbol id width is a strict function of the table size, for the
    // same canonicality reason.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrence_symbols = {0, 0};

    auto bytes = archive::encode(blob);
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
    blob.symbol_hashes = {111, 222};
    blob.relation_offsets = {0, 0, 0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.symbol_hashes = {111, 111};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(DuplicateVariantRejected) {
    // Liveness and compaction select variants by identity; a duplicated
    // entry would make every copy live at once, and entries masked only to the
    // extra id would serve and survive with no contribution owning them.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
    blob.variants = {1, 2};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};

    auto bytes_of = [&] {
        return archive::encode(blob);
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
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 1};
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrence_symbols = {0};
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Base) | index::payload_bit};
    blob.relations.packed = {index::pack_range(4, 3)};
    blob.relation_target_ranks = {0};
    blob.relation_extent_ranks = {0};
    blob.relation_targets = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occurrence_symbols = {5};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.occurrence_symbols = {0};

    blob.relation_targets = {5};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(RelationPayloadColumnsRejected) {
    // Readers place a relation's payload by the payload count before its
    // block and the payload bits inside it, and read an extent or a target
    // by its kind: counts or columns out of step would serve another
    // relation's payload, or one payload's bits as the other.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaaaaaaaaaaaaaa");
    blob.variants = {1};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 1};
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Definition) |
                           index::payload_bit};
    blob.relations.packed = {index::pack_range(4, 3)};
    blob.relation_target_ranks = {0};
    blob.relation_extent_ranks = {0};
    blob.relation_extent_heads = {4};
    blob.relation_extent_lengths = {8};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    auto extent_of = [&](const index::Shard& shard) {
        LocalSourceRange extent;
        shard.relations_of(111, RelationKind::Definition, [&](const index::Relation& relation) {
            extent = relation.extent();
            return false;
        });
        return extent;
    };
    auto shard = make_shard(bytes_of());
    ZASSERT(shard.loaded());
    ZASSERT(extent_of(shard) == LocalSourceRange{0, 8});

    blob.relation_extent_ranks = {1};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.relation_extent_ranks = {0};

    // An extent without the payload bit owning it, a target in its place.
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Definition)};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Base) | index::payload_bit};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.relation_kinds = {static_cast<std::uint8_t>(RelationKind::Definition) |
                           index::payload_bit};

    // An extent the u16 columns hold is not escaped; one beginning past
    // its name is.
    blob.relation_extent_heads = {index::extent_escape};
    blob.relation_extent_lengths = {0};
    blob.relation_far_extents = {0};
    blob.relation_far_begins = {0};
    blob.relation_far_ends = {8};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.relation_far_begins = {5};
    shard = make_shard(bytes_of());
    ZASSERT(shard.loaded());
    ZASSERT(extent_of(shard) == LocalSourceRange{5, 8});
}

ZEST_CASE(OwnerlessMaskRejected) {
    // A mask owning no stored variant serves its entry unconditionally while
    // every variant is live (LiveFilter's live.all fast path never consults
    // it), vanishes once any variant dies, and the next compaction erases
    // it for real — so it must reject the blob at load.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
    blob.variants = {1, 2};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrence_symbols = {0};
    blob.occurrences.masks8 = {0b01};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    // An empty mask, then one whose only bit lies past the variant table.
    blob.occurrences.masks8 = {0};
    ZASSERT(!make_shard(bytes_of()).loaded());
    blob.occurrences.masks8 = {0b100};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The mask column is the narrowest the variant count allows.
    blob.occurrences.masks8 = {};
    blob.occurrences.masks32 = {0b01};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The wider encodings are bounded alike.
    for(std::uint32_t i = 3; i <= 12; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occurrences.masks32 = {};
    blob.occurrences.masks16 = {1 << 11};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.occurrences.masks16 = {1 << 13};
    ZASSERT(!make_shard(bytes_of()).loaded());
    for(std::uint32_t i = 13; i <= 40; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occurrences.masks16 = {};
    blob.occurrences.masks64 = {1};
    ZASSERT(make_shard(bytes_of()).loaded());
    blob.occurrences.masks64 = {std::uint64_t(1) << 45};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // And roaring masks: decodable but empty, or holding only dropped ids.
    for(std::uint32_t i = 41; i <= 70; i += 1) {
        blob.variants.push_back(i);
    }
    blob.occurrences.masks64 = {};
    auto set_mask = [&](const clice::Bitmap& mask) {
        blob.occurrences.roaring.clear();
        for(auto byte: index::write_bitmap(mask)) {
            blob.occurrences.roaring.push_back(static_cast<std::uint8_t>(byte));
        }
        blob.occurrences.roaring_offsets = {
            0,
            static_cast<std::uint32_t>(blob.occurrences.roaring.size())};
        blob.relations.roaring_offsets = {0};
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
    // Roaring entry masks gate liveness and are rewritten by compaction; a
    // slice failing decode would read the entry as dead and the next
    // compaction would erase it for real, every manifest still fresh — so
    // an undecodable slice must reject the blob at load.
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    fill_content(blob, "aaaa");
    for(std::uint32_t i = 1; i <= 65; i += 1) {
        blob.variants.push_back(i);
    }
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    set_occurrences(blob,
                    {
                        {0, 3}
    });
    blob.occurrence_symbols = {0};

    clice::Bitmap mask;
    mask.add(2);
    for(auto byte: index::write_bitmap(mask)) {
        blob.occurrences.roaring.push_back(static_cast<std::uint8_t>(byte));
    }
    blob.occurrences.roaring_offsets = {
        0,
        static_cast<std::uint32_t>(blob.occurrences.roaring.size())};
    blob.relations.roaring_offsets = {0};

    auto bytes_of = [&] {
        return archive::encode(blob);
    };
    ZASSERT(make_shard(bytes_of()).loaded());

    blob.occurrences.roaring = {0xff, 0xff, 0xff};
    blob.occurrences.roaring_offsets = {0, 3};
    ZASSERT(!make_shard(bytes_of()).loaded());

    // The payload offset in the image's header, which the in-place view
    // follows, pointing past the slice.
    blob.occurrences.roaring.clear();
    for(auto byte: index::write_bitmap(mask)) {
        blob.occurrences.roaring.push_back(static_cast<std::uint8_t>(byte));
    }
    blob.occurrences.roaring[12] = 0xff;
    blob.occurrences.roaring_offsets = {
        0,
        static_cast<std::uint32_t>(blob.occurrences.roaring.size())};
    ZASSERT(!make_shard(bytes_of()).loaded());
}

ZEST_CASE(RebindSwapsIdenticalBytes) {
    build_index("int rebind_value() { return 1; }\n");
    auto bytes = focus_blob();
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
