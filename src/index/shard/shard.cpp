module clice;

import :index.columns;
import :index.format;
import :index.shard;
import :index.shard.layout;
import :index.shard.tables;
import :semantic.token_class;
import :support.logging;
import :syntax.position;

namespace clice::index {

/// The live mask against one side's mask columns, resolved once per query.
struct LiveFilter {
    /// Fast path: every stored variant is live, no per-entry filtering.
    bool all;
    MaskEncoding encoding;
    std::uint64_t bits;
    const Bitmap* big;
    const MaskColumnsView* masks;

    bool operator()(std::uint32_t entry) const {
        if(all) {
            return true;
        }
        if(encoding == MaskEncoding::Roaring) {
            return bitmap_of(*masks, entry).intersect(*big);
        }
        return (mask_bits(encoding, *masks, entry) & bits) != 0;
    }
};

namespace {

/// The line table's lengths in line order: an escaped length is the next
/// of the long line lengths.
struct LineLengths {
    const Root& root;

    std::uint32_t escaped = 0;

    std::uint32_t next(std::uint32_t line) {
        auto length = root.line_lengths[line];
        if(length != length_escape) {
            return length;
        }
        auto long_length = root.long_line_lengths[escaped];
        escaped += 1;
        return long_length;
    }
};

// A variant is identified by its hash everywhere (set_live, the merge
// keep filter), so stored hashes must be unique: occurrences and
// relations owned only by a duplicated entry would serve and survive
// compaction with no contribution owning them.
CheckResult variants_ok(const Root& root) {
    auto& variants = root.variants;
    llvm::SmallVector<VariantHash> sorted(variants.begin(), variants.end());
    ranges::sort(sorted);
    if(ranges::adjacent_find(sorted) != sorted.end()) {
        return std::unexpected("duplicate variant identity");
    }
    return {};
}

// The line table reconstructs every line start by prefix sum, so it must
// both pair with its escape table and add up to exactly the content size —
// a drifted sum would shift every position mapping below the corruption.
// Position mapping reads a listed non-ASCII line's text in place of the
// line: it must be exactly the line's text without its end, and hold a
// non-ASCII byte — a pure-ASCII line is the absent entry's to spell.
CheckResult line_table_ok(const Root& root) {
    auto& line_lengths = root.line_lengths;
    auto& long_lines = root.long_lines;
    if(line_lengths.empty() ||
       !escapes_ok(line_lengths, length_escape, long_lines, root.long_line_lengths.size())) {
        return std::unexpected("line table does not pair with its escape table");
    }
    // A "\r\n" ending needs a line holding at least those two bytes and a
    // line after it.
    llvm::ArrayRef<std::uint64_t> crlf_lines = root.crlf_lines;
    if(!crlf_lines.empty() &&
       (crlf_lines.back() == 0 || crlf_lines.size() > (line_lengths.size() + 63) / 64)) {
        return std::unexpected("CRLF line table is not canonical");
    }
    auto& utf8_lines = root.utf8_lines;
    auto& utf8_text = root.utf8_text;
    auto& utf8_ends = root.utf8_text_ends;
    if(utf8_ends.size() != utf8_lines.size() || !covering_ends(utf8_ends, utf8_text.size())) {
        return std::unexpected("non-ASCII line columns do not line up");
    }
    std::uint64_t line_sum = 0;
    std::uint32_t utf8_cursor = 0;
    auto line_count = static_cast<std::uint32_t>(line_lengths.size());
    LineLengths lengths{.root = root};
    for(std::uint32_t line = 0; line < line_count; line += 1) {
        auto length = lengths.next(line);
        if(line_lengths[line] == length_escape && length < length_escape) {
            return std::unexpected("escaped line length below the escape threshold");
        }
        auto last = line + 1 == line_count;
        auto crlf = ends_crlf(crlf_lines, line);
        if(crlf && (length < 2 || last)) {
            return std::unexpected("CRLF bit on a line that cannot end in CRLF");
        }
        if(utf8_cursor < utf8_lines.size() && utf8_lines[utf8_cursor] == line) {
            auto text = slice(utf8_text, utf8_ends, utf8_cursor);
            if(text.size() != (last ? length : length - (crlf ? 2 : 1)) || text.contains('\n') ||
               lsp::is_ascii({text.data(), text.size()})) {
                return std::unexpected("a non-ASCII line's text does not match its line");
            }
            utf8_cursor += 1;
        }
        line_sum += length;
    }
    if(utf8_cursor != utf8_lines.size()) {
        return std::unexpected("non-ASCII lines are not ascending lines of the table");
    }
    if(line_sum != root.content_size) {
        return std::unexpected("line lengths do not add up to the content size");
    }
    return {};
}

// Strictly sorted symbol hashes: lookups lower-bound the hash column and
// read only the first match's slices, so a duplicated hash would strand
// the later id's relations and local name unreachably.
CheckResult symbol_table_ok(const Root& root) {
    auto& symbol_hashes = root.symbol_hashes;
    auto table = RelationTable::of(root);
    auto relations = table.size();
    if(auto ok = ascending_ids_ok(table.offsets, symbol_hashes.size() + 1, relations + 1, false);
       !ok) {
        return ok;
    }
    if(table.offsets[0] != 0 || table.offsets[symbol_hashes.size()] != relations) {
        return std::unexpected("relation offsets are not a partition of the relations");
    }
    // FIXME: a reserved hash (~0, ~0-1) is not rejected here as the other hash
    // columns reject it: corrupt bytes ending in one assert as a DenseMap key in
    // collect_locals.
    if(!strictly_ascending(symbol_hashes)) {
        return std::unexpected("symbol hashes are not strictly ascending");
    }
    return {};
}

CheckResult range_encodings_ok(const Root& root) {
    auto& content_size = root.content_size;
    if(auto ok = range_encoding_ok(root.occurrences, content_size, RangeEncoding::Delta); !ok) {
        return ok;
    }
    return range_encoding_ok(root.relation_explicit_ranges,
                             content_size,
                             unsorted_encoding(content_size));
}

// occurrences_at binary-searches the decoded end column and stops its
// containment walk on begin order, which the delta encoding proves; ends out
// of order (a corrupt escaped end included) would silently miss or
// misresolve occurrences on every query, forever — reject the blob so it is
// rebuilt instead.
// The merge also two-way merges occurrences under the full (begin, end, symbol)
// key with equal-key occurrences combined at write time, so equal ranges must
// carry strictly ascending symbols — symbol_hashes is strictly sorted, so
// id order stands in for hash order.
CheckResult occurrences_ok(const Root& root) {
    auto table = OccurrenceTable::of(root);
    if(auto ok = ids_ok(table.symbols, table.size(), root.symbol_hashes.size()); !ok) {
        return ok;
    }
    LocalSourceRange prev{0, 0};
    std::uint32_t previous_symbol = 0;
    CheckResult result;
    for_each_range(table.columns, [&](std::uint32_t entry, LocalSourceRange range) {
        auto symbol = table.symbols[entry];
        if(range.end < prev.end) {
            result = std::unexpected("occurrence ends are out of order");
        } else if(entry != 0 && range == prev && symbol <= previous_symbol) {
            result = std::unexpected("occurrences of one range are not strictly ascending");
        }
        prev = range;
        previous_symbol = symbol;
        return result.has_value();
    });
    return result;
}

CheckResult relations_ok(const Root& root) {
    auto table = RelationTable::of(root);
    auto occurrences = OccurrenceTable::of(root);
    auto& content_size = root.content_size;
    auto& symbol_hashes = root.symbol_hashes;
    auto& extent_heads = root.relation_extent_heads;
    auto& extent_lengths = root.relation_extent_lengths;
    auto& far_begins = root.relation_far_begins;

    // Readers place a payload, a range code and an explicit range by its
    // block's counts and the entries before it, trusting both: one wrong
    // count would hand every later relation of the block another relation's
    // payload or range.
    auto blocks = (table.size() + rank_block - 1) / rank_block;
    if(auto ok = ids_ok(table.target_ranks, blocks, table.targets.size() + 1); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.extent_ranks, blocks, extent_heads.size() + 1); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.range_ranks, blocks, table.codes.size() + 1); !ok) {
        return ok;
    }
    auto explicit_count = entry_count(table.explicit_ranges);
    if(auto ok = ids_ok(table.explicit_ranks, blocks, explicit_count + 1); !ok) {
        return ok;
    }
    if(extent_lengths.size() != extent_heads.size() ||
       root.relation_far_ends.size() != far_begins.size() ||
       !escapes_ok(extent_heads, extent_escape, root.relation_far_extents, far_begins.size())) {
        return std::unexpected("relation extent columns do not line up");
    }
    // An occurrence code names the first occurrence of its range, and an
    // explicit range is one no occurrence has: one range, one encoding.
    std::vector<LocalSourceRange> spans;
    spans.reserve(occurrences.size());
    for_each_range(occurrences.columns, [&](std::uint32_t, LocalSourceRange range) {
        spans.push_back(range);
        return true;
    });
    auto bounds = [](LocalSourceRange range) {
        return std::pair(range.begin, range.end);
    };
    auto spanned = [&](LocalSourceRange range) {
        return ranges::binary_search(spans, bounds(range), {}, bounds);
    };

    // Entries of one relation group two-way merge under the full (kind,
    // begin, end, payload) key with equal-key entries combined at write time,
    // so the key must ascend strictly within each group — out-of-order or
    // repeated entries would mis-merge silently instead of being rejected.
    std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint64_t> previous{};
    std::uint32_t group = 0;
    RelationTable::Cursor cursor;
    for(std::uint32_t entry = 0; entry < table.size(); entry += 1) {
        if(entry % rank_block == 0) {
            auto block = entry / rank_block;
            if(table.target_ranks[block] != cursor.target ||
               table.extent_ranks[block] != cursor.extent ||
               table.range_ranks[block] != cursor.code ||
               table.explicit_ranks[block] != cursor.explicit_range) {
                return std::unexpected("relation block counts are wrong");
            }
        }
        auto payload = cursor;
        auto at = table.advance(cursor, entry);
        if(!at) {
            return std::unexpected("relation range code is not one");
        }
        LocalSourceRange range;
        switch(at->code) {
            case range_none: {
                // Pair relations carry no range of their own; every other
                // kind is written with a real range.
                if(!table.kind_of(entry).between_symbols()) {
                    return std::unexpected("no range on a relation that carries one");
                }
                break;
            }
            case range_explicit: {
                if(at->index >= explicit_count) {
                    return std::unexpected("explicit relation range past its column");
                }
                range = range_of(table.explicit_ranges, at->index);
                if(spanned(range)) {
                    return std::unexpected("explicit relation range is not the canonical one");
                }
                break;
            }
            default: {
                if(at->index >= spans.size() ||
                   (at->index != 0 && spans[at->index - 1] == spans[at->index])) {
                    return std::unexpected("relation occurrence is not the canonical one");
                }
                range = spans[at->index];
                break;
            }
        }
        if(payload.target != cursor.target &&
           (payload.target >= table.targets.size() ||
            table.targets[payload.target] >= symbol_hashes.size())) {
            return std::unexpected("relation target past its table");
        }
        if(payload.extent != cursor.extent && payload.extent >= extent_heads.size()) {
            return std::unexpected("relation extent past its column");
        }
        auto relation = table.with_payload(entry, payload, range);
        if(payload.extent != cursor.extent) {
            auto k = payload.extent;
            auto extent = relation.extent();
            bool escaped = extent_heads[k] == extent_escape;
            if(escaped ? extent_lengths[k] != 0 || fits_extent_columns(relation.range, extent)
                       : extent_heads[k] > relation.range.begin) {
                return std::unexpected("relation extent is not the canonical one");
            }
            if(extent.end < extent.begin || extent.end > content_size) {
                return std::unexpected("definition range past the content");
            }
        }
        while(entry >= table.offsets[group + 1]) {
            group += 1;
        }
        std::tuple key{static_cast<std::uint8_t>(relation.kind),
                       relation.range.begin,
                       relation.range.end,
                       relation.target};
        if(entry != table.offsets[group] && key <= previous) {
            return std::unexpected("relations of one symbol are not strictly ascending");
        }
        previous = key;
    }
    if(cursor.code != table.codes.size() || cursor.explicit_range != explicit_count) {
        return std::unexpected("relation ranges do not match the relation count");
    }
    if(table.targets.bytes.size() != std::size_t(cursor.target) * table.targets.width ||
       cursor.extent != extent_heads.size()) {
        return std::unexpected("relation payloads do not match their columns");
    }
    return {};
}

CheckResult locals_ok(const Root& root) {
    auto table = LocalTable::of(root);
    auto count = table.size();
    auto& symbol_hashes = root.symbol_hashes;
    auto table_size = symbol_hashes.size();
    if(auto ok = ascending_ids_ok(table.symbols, count, table_size, true); !ok) {
        return ok;
    }
    auto& token_kinds = root.local_token_kinds;
    if(root.local_kinds.size() != count || root.local_scopes.size() != count ||
       root.local_flags.size() != count || token_kinds.size() != count ||
       root.local_token_modifiers.size() != count || !all_token_kinds(token_kinds)) {
        return std::unexpected("local symbol columns do not line up");
    }
    if(auto ok = ends_ok(table.name_ends, count, root.local_names.size()); !ok) {
        return ok;
    }
    // The few locals with arguments or a signature: ascending positions,
    // each text non-empty — an empty one is the absent entry's to spell.
    auto texts_ok = [&](const IdColumnView& of,
                        std::string_view text,
                        const IdColumnView& ends) -> CheckResult {
        if(auto ok = ascending_ids_ok(of, of.size(), count, true); !ok) {
            return ok;
        }
        if(auto ok = ends_ok(ends, of.size(), text.size()); !ok) {
            return ok;
        }
        for(std::uint32_t i = 0; i < of.size(); i += 1) {
            if(ends.slice(text, i).empty()) {
                return std::unexpected("an empty local argument or signature stored");
            }
        }
        return {};
    };
    if(auto ok = texts_ok(table.args_of, root.local_args, table.args_ends); !ok) {
        return ok;
    }
    if(auto ok = texts_ok(table.signatures_of, root.local_signatures, table.signature_ends); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.parents, count, table_size + 1); !ok) {
        return ok;
    }
    // A local symbol's parent becomes a DenseSet key in a query's container
    // walk, where the sentinel values corrupt or assert.
    for(std::uint32_t k = 0; k < count; k += 1) {
        if(table.parents[k] != 0 && reserved_key(symbol_hashes[table.parents[k] - 1])) {
            return std::unexpected("reserved local parent hash");
        }
    }
    return {};
}

// Beyond the per-encoding column shape, every mask must own at least one
// stored variant and no bits past the variant table: an ownerless entry
// serves unconditionally while every stored variant is live (the
// LiveFilter fast path skips the mask), vanishes once any variant dies,
// and the next compaction erases it for real — every manifest still
// fresh throughout.
CheckResult masks_ok(const Root& root) {
    auto variant_count = root.variants.size();
    auto encoding = encoding_of(variant_count);
    std::pair<const MaskColumnsView*, std::uint32_t> sides[] = {
        {&root.occurrence_masks, entry_count(root.occurrences)},
        {&root.relation_masks,   root.relation_kinds.size()   },
    };
    for(auto [side, count]: sides) {
        auto expected = [&](MaskEncoding column) -> std::uint32_t {
            if(column != encoding) {
                return 0;
            }
            return column == MaskEncoding::Roaring ? count + 1 : count;
        };
        auto& roaring_offsets = side->roaring_offsets;
        auto& roaring = side->roaring;
        if(side->masks8.size() != expected(MaskEncoding::U8) ||
           side->masks16.size() != expected(MaskEncoding::U16) ||
           side->masks32.size() != expected(MaskEncoding::U32) ||
           side->masks64.size() != expected(MaskEncoding::U64) ||
           roaring_offsets.size() != expected(MaskEncoding::Roaring) ||
           (encoding != MaskEncoding::Roaring && !roaring.empty())) {
            return std::unexpected("mask encoding is not the canonical one");
        }
        if(encoding == MaskEncoding::Single) {
            continue;
        }
        if(encoding != MaskEncoding::Roaring) {
            auto stray = variant_count < 64 ? ~std::uint64_t(0) << variant_count : 0;
            for(std::uint32_t entry = 0; entry < count; entry += 1) {
                auto mask = mask_bits(encoding, *side, entry);
                if(mask == 0 || (mask & stray) != 0) {
                    return std::unexpected("entry mask owns no stored variant");
                }
            }
            continue;
        }
        if(!ranges::is_sorted(roaring_offsets) || roaring_offsets.back() != roaring.size() ||
           (count != 0 && roaring_offsets.front() != 0)) {
            return std::unexpected("mask encoding is not the canonical one");
        }
        // A slice failing decode would read as an empty mask — the
        // ownerless-entry corruption above in another coat. Prove each
        // slice decodes once here so queries stay check-free and the blob
        // rebuilds instead.
        for(std::uint32_t entry = 0; entry < count; entry += 1) {
            auto begin = roaring_offsets[entry];
            auto mask = read_bitmap(roaring.data() + begin, roaring_offsets[entry + 1] - begin);
            if(!mask || mask->isEmpty() || mask->maximum() >= variant_count) {
                return std::unexpected("entry mask does not decode or owns no stored variant");
            }
        }
    }
    return {};
}

/// Structural verification does not constrain field values; everything the
/// readers dereference through raw column pointers or binary-search must be
/// proven in-bounds and in order here, once, so queries stay check-free.
/// The checks are also canonicality checks: every self-describing choice
/// (range encoding, id width, non-ASCII lines, mask encoding) is a strict
/// function of the data, so one logical blob has exactly one encoding and
/// its byte hash is a usable identity. Later checks decode through columns
/// earlier ones bounded, so the order matters.
CheckResult validate(const Root& root) {
    constexpr std::array checks = {
        variants_ok,
        line_table_ok,
        symbol_table_ok,
        range_encodings_ok,
        occurrences_ok,
        relations_ok,
        locals_ok,
        masks_ok,
    };
    for(auto check: checks) {
        if(auto ok = check(root); !ok) {
            return ok;
        }
    }
    return {};
}

}  // namespace

Shard::Shard(std::unique_ptr<llvm::MemoryBuffer> buffer) : buffer(std::move(buffer)) {
    blob_hash = llvm::xxh3_64bits(this->buffer->getBuffer());
}

Shard Shard::from_bytes(llvm::StringRef data) {
    return from_buffer(llvm::MemoryBuffer::getMemBuffer(data, "", false));
}

bool Shard::rebind(std::unique_ptr<llvm::MemoryBuffer> replacement) {
    return rebind_buffer(buffer, std::move(replacement));
}

Shard Shard::from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!buffer) {
        return {};
    }
    buffer = in_place_buffer(std::move(buffer));

    // Stale or corrupt bytes (an older build's cache directory) must never
    // crash the server or be misread: deep structural verification first,
    // then the format-version gate, then the cross-field checks the raw
    // column readers rely on. Anything failing loads as "not on disk" and
    // the background indexer rebuilds it.
    auto root = verify_root<ShardBlob>(buffer->getBuffer(), "shard blob");
    if(!root) {
        return {};
    }
    if(auto ok = validate(*root); !ok) {
        LOG_DEBUG("Rejecting shard blob: {}", ok.error());
        return {};
    }
    return Shard(std::move(buffer));
}

std::uint64_t Shard::content_hash() const {
    if(!buffer) {
        return 0;
    }
    return view_of(buffer->getBuffer()).content_hash;
}

std::uint32_t Shard::content_size() const {
    if(!buffer) {
        return 0;
    }
    return view_of(buffer->getBuffer()).content_size;
}

bool Shard::matches_content(llvm::StringRef text) const {
    return matches_content(text.size(), llvm::xxh3_64bits(text));
}

bool Shard::matches_content(std::uint64_t size, std::uint64_t hash) const {
    return loaded() && size == content_size() && hash == content_hash();
}

std::vector<VariantHash> Shard::variants() const {
    if(!buffer) {
        return {};
    }
    auto& stored = view_of(buffer->getBuffer()).variants;
    if(stored.empty()) {
        return {blob_hash};
    }
    return {stored.begin(), stored.end()};
}

bool Shard::has_variant(VariantHash hash) const {
    if(!buffer) {
        return false;
    }
    auto& stored = view_of(buffer->getBuffer()).variants;
    if(stored.empty()) {
        return hash == blob_hash;
    }
    return ranges::contains(stored, hash);
}

void Shard::set_live(llvm::ArrayRef<VariantHash> live_hashes) {
    live = {};
    if(!buffer) {
        return;
    }

    auto stored = variants();
    std::uint32_t matched = 0;
    Live next;
    for(std::uint32_t id = 0; id < stored.size(); id += 1) {
        if(!ranges::contains(live_hashes, stored[id])) {
            continue;
        }
        matched += 1;
        if(id < 64) {
            next.bits |= std::uint64_t(1) << id;
        }
        next.big.add(id);
    }
    next.all = matched == stored.size();
    live = std::move(next);
}

bool Shard::has_dead_variants() const {
    return loaded() && !live.all;
}

LiveFilter Shard::live_filter(const MaskColumnsView& masks) const {
    return {
        .all = live.all,
        .encoding = encoding_of(view_of(buffer->getBuffer())),
        .bits = live.bits,
        .big = &live.big,
        .masks = &masks,
    };
}

void Shard::occurrences_at(std::uint32_t offset,
                           llvm::function_ref<bool(const Occurrence&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = OccurrenceTable::of(view_of(buffer->getBuffer()));
    auto is_live = live_filter(table.masks);

    // Binary search the first occurrence whose end reaches the offset, then walk
    // while occurrences contain it. Occurrence ranges are name-token spans,
    // pairwise disjoint or identical, so under (begin, end) order the end
    // column is monotonic too.
    auto first =
        partition_point(table.columns, [&](LocalSourceRange range) { return range.end < offset; });

    // A cursor between two tokens belongs to the one starting at it, so
    // occurrences the offset only touches at their right edge yield after every
    // occurrence it is properly inside — `a+b` with the cursor before `b` must
    // resolve to `b`, not `+`, while a cursor right after a lone token
    // still hits that token.
    llvm::SmallVector<Occurrence, 2> edges;
    bool stopped = false;
    for_each_range(table.columns, first, [&](std::uint32_t entry, LocalSourceRange range) {
        if(!range.touches(offset)) {
            return false;
        }
        if(!is_live(entry)) {
            return true;
        }
        if(range.end == offset && range.begin != offset) {
            edges.push_back(table.at(entry, range));
            return true;
        }
        stopped = !callback(table.at(entry, range));
        return !stopped;
    });
    if(stopped) {
        return;
    }
    for(auto& edge: edges) {
        if(!callback(edge)) {
            return;
        }
    }
}

namespace {

/// Visit the relations [begin_entry, end_entry) that pass `is_live`, `cursor`
/// placed at the first; false when the callback stopped the walk.
bool visit_relation_entries(const RelationTable& table,
                            std::uint32_t begin_entry,
                            std::uint32_t end_entry,
                            RelationTable::Cursor& cursor,
                            const LiveFilter& is_live,
                            llvm::function_ref<bool(const Relation&)> callback) {
    for(auto entry = begin_entry; entry < end_entry; entry += 1) {
        auto relation = table.decode(entry, cursor);
        if(is_live(entry) && !callback(relation)) {
            return false;
        }
    }
    return true;
}

}  // namespace

void Shard::relations_of(SymbolHash symbol,
                         RelationKind kind,
                         llvm::function_ref<bool(const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto& root = view_of(buffer->getBuffer());
    auto table = RelationTable::of(root);
    auto found = sorted_find(root.symbol_hashes, symbol);
    if(!found) [[unlikely]] {
        return;
    }
    auto id = *found;
    auto cursor = table.cursor_at(table.offsets[id]);
    visit_relation_entries(table,
                           table.offsets[id],
                           table.offsets[id + 1],
                           cursor,
                           live_filter(table.masks),
                           [&](const Relation& relation) {
                               if(RelationKind(relation.kind) != kind) {
                                   return true;
                               }
                               return callback(relation);
                           });
}

bool Shard::has_relation(SymbolHash symbol, RelationKind kind) const {
    bool found = false;
    relations_of(symbol, kind, [&](const Relation&) {
        found = true;
        return false;
    });
    return found;
}

void Shard::for_each_occurrence(llvm::function_ref<bool(const Occurrence&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = OccurrenceTable::of(view_of(buffer->getBuffer()));
    auto is_live = live_filter(table.masks);
    for_each_range(table.columns, [&](std::uint32_t entry, LocalSourceRange range) {
        return !is_live(entry) || callback(table.at(entry, range));
    });
}

void
    Shard::for_each_relation(llvm::function_ref<bool(SymbolHash, const Relation&)> callback) const {
    if(!buffer) {
        return;
    }
    auto& root = view_of(buffer->getBuffer());
    auto table = RelationTable::of(root);
    auto is_live = live_filter(table.masks);
    auto& symbol_hashes = root.symbol_hashes;
    RelationTable::Cursor cursor;
    for(std::uint32_t id = 0; id < symbol_hashes.size(); id += 1) {
        auto hash = symbol_hashes[id];
        bool completed = visit_relation_entries(
            table,
            table.offsets[id],
            table.offsets[id + 1],
            cursor,
            is_live,
            [&](const Relation& relation) { return callback(hash, relation); });
        if(!completed) {
            return;
        }
    }
}

namespace {

/// What the relations of symbol `id` say of it; none when no relation is
/// its own or targets it.
std::optional<Mention> mention_of(const Root& root,
                                  const RelationTable& relations,
                                  std::uint32_t id,
                                  bool targeted,
                                  bool local) {
    // The table also holds the parents of its local symbols, which no entry
    // need name.
    auto begin = relations.offsets[id];
    auto end = relations.offsets[id + 1];
    if(begin == end && !targeted) {
        return std::nullopt;
    }
    return Mention{
        .symbol = root.symbol_hashes[id],
        .local = local,
        .declares = begin != end && relations.kind_of(begin).declares(),
        .defines = relations.defines(id),
    };
}

}  // namespace

void Shard::for_each_mention(llvm::function_ref<void(const Mention&)> callback) const {
    if(!buffer) {
        return;
    }
    auto& root = view_of(buffer->getBuffer());
    auto relations = RelationTable::of(root);
    auto locals = LocalTable::of(root);
    llvm::BitVector targeted(root.symbol_hashes.size());
    for(std::uint32_t i = 0; i < relations.targets.size(); i += 1) {
        targeted.set(relations.targets[i]);
    }
    std::uint32_t next_local = 0;
    for(std::uint32_t id = 0; id < root.symbol_hashes.size(); id += 1) {
        bool local = next_local < locals.size() && locals.symbols[next_local] == id;
        if(local) {
            next_local += 1;
        }
        if(auto mention = mention_of(root, relations, id, targeted.test(id), local)) {
            callback(*mention);
        }
    }
}

std::optional<Mention> Shard::mention(SymbolHash symbol) const {
    if(!buffer) {
        return std::nullopt;
    }
    auto& root = view_of(buffer->getBuffer());
    auto id = sorted_find(root.symbol_hashes, symbol);
    if(!id) {
        return std::nullopt;
    }
    auto relations = RelationTable::of(root);
    bool targeted = false;
    if(relations.offsets[*id] == relations.offsets[*id + 1]) {
        for(std::uint32_t i = 0; i < relations.targets.size() && !targeted; i += 1) {
            targeted = relations.targets[i] == *id;
        }
    }
    return mention_of(root,
                      relations,
                      *id,
                      targeted,
                      LocalTable::of(root).symbols.find(*id).has_value());
}

namespace {

/// Local symbol `k` as the shard hands it out: defined when the stored
/// entries define it, which the table itself does not say.
SymbolView local_symbol(const LocalTable& table, const RelationTable& relations, std::uint32_t k) {
    auto symbol = table.at(k);
    if(relations.defines(table.symbols[k])) {
        symbol.flags |= SymbolFlags::HasDefinition;
    }
    return symbol;
}

}  // namespace

std::optional<SymbolView> Shard::find_symbol(SymbolHash hash) const {
    if(!buffer) {
        return std::nullopt;
    }
    auto& root = view_of(buffer->getBuffer());
    auto table = LocalTable::of(root);
    auto position = sorted_find(root.symbol_hashes, hash);
    if(!position) {
        return std::nullopt;
    }
    auto local = table.symbols.find(*position);
    if(!local) {
        return std::nullopt;
    }
    return local_symbol(table, RelationTable::of(root), *local);
}

void Shard::for_each_local_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolView&)> callback) const {
    if(!buffer) {
        return;
    }
    auto& root = view_of(buffer->getBuffer());
    auto table = LocalTable::of(root);
    auto relations = RelationTable::of(root);
    for(std::uint32_t k = 0; k < table.size(); k += 1) {
        if(!callback(table.hash_of(k), local_symbol(table, relations, k))) {
            return;
        }
    }
}

std::span<const std::uint32_t> Shard::line_starts() const {
    if(!buffer) {
        return {};
    }
    if(line_starts_cache.empty()) {
        auto& root = view_of(buffer->getBuffer());
        auto line_count = static_cast<std::uint32_t>(root.line_lengths.size());
        line_starts_cache.reserve(line_count);
        LineLengths lengths{.root = root};
        std::uint32_t start = 0;
        for(std::uint32_t line = 0; line < line_count; line += 1) {
            line_starts_cache.push_back(start);
            start += lengths.next(line);
        }
    }
    return line_starts_cache;
}

PositionMap Shard::positions() const {
    if(!buffer) {
        return {0, {}, {}};
    }
    auto& root = view_of(buffer->getBuffer());
    return {content_size(),
            line_starts(),
            root.crlf_lines,
            root.utf8_lines,
            llvm::StringRef(root.utf8_text),
            root.utf8_text_ends};
}

}  // namespace clice::index
