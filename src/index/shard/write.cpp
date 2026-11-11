module clice;

import :compile.inactive_regions;
import :index.columns;
import :index.format;
import :index.shard;
import :index.shard.layout;
import :index.shard.tables;
import :support.text;

namespace clice::index {

namespace {

/// Working entries during a write: the entry as the readers hand it out plus
/// its variant mask. Masks stay wide; the encoding is chosen at emit time from
/// the final variant count.
template <typename MaskT>
struct OccurrenceEntry {
    Occurrence occurrence;
    MaskT mask;
};

template <typename MaskT>
struct RelationEntry {
    Relation relation;
    MaskT mask;
};

/// One symbol's relation entries, sorted by (kind, begin, end, payload).
template <typename MaskT>
struct RelationGroup {
    SymbolHash symbol;
    std::vector<RelationEntry<MaskT>> entries;
};

/// Everything a write accumulates before choosing column encodings.
template <typename MaskT>
struct MergedEntries {
    std::vector<OccurrenceEntry<MaskT>> occurrences;
    /// Groups sorted by symbol hash.
    std::vector<RelationGroup<MaskT>> relations;
};

constexpr auto occurrence_key = [](const auto& entry) {
    return std::tuple(entry.occurrence.range.begin,
                      entry.occurrence.range.end,
                      entry.occurrence.target);
};

/// The kind as the column stores it: the serialized order the readers rely on.
constexpr auto relation_key = [](const auto& entry) {
    return std::tuple(static_cast<std::uint8_t>(entry.relation.kind),
                      entry.relation.range.begin,
                      entry.relation.range.end,
                      entry.relation.target);
};

constexpr auto group_key = [](const auto& group) {
    return group.symbol;
};

template <typename MaskT>
MaskT single_bit(std::uint32_t id) {
    if constexpr(std::same_as<MaskT, std::uint64_t>) {
        return std::uint64_t(1) << id;
    } else {
        MaskT mask;
        mask.add(id);
        return mask;
    }
}

template <typename MaskT>
bool mask_empty(const MaskT& mask) {
    if constexpr(std::same_as<MaskT, std::uint64_t>) {
        return mask == 0;
    } else {
        return mask.isEmpty();
    }
}

/// The old blob's mask of one entry, remapped through old-id -> new-id (a
/// dropped variant's bit vanishes; an all-dropped entry reads as empty and
/// is skipped by the caller).
template <typename MaskT>
MaskT remap_mask(MaskEncoding encoding,
                 const MaskColumnsView& masks,
                 std::uint32_t entry,
                 llvm::ArrayRef<std::int64_t> id_map) {
    MaskT result{};
    auto apply = [&](std::uint32_t old_id) {
        if(id_map[old_id] >= 0) {
            result |= single_bit<MaskT>(static_cast<std::uint32_t>(id_map[old_id]));
        }
    };
    auto apply_bits = [&](std::uint64_t bits) {
        while(bits != 0) {
            apply(static_cast<std::uint32_t>(std::countr_zero(bits)));
            bits &= bits - 1;
        }
    };
    if(encoding == MaskEncoding::Roaring) {
        for(auto id: bitmap_of(masks, entry)) {
            apply(id);
        }
    } else {
        apply_bits(mask_bits(encoding, masks, entry));
    }
    return result;
}

/// Two-way merge of runs sorted under `key`; entries with equal keys are
/// one entry and OR their masks — the cross-variant dedup.
template <typename Entry, typename Key>
void merge_sorted(std::vector<Entry> old_entries,
                  std::vector<Entry> fresh_entries,
                  Key key,
                  std::vector<Entry>& out) {
    out.reserve(old_entries.size() + fresh_entries.size());
    auto lhs = old_entries.begin();
    auto rhs = fresh_entries.begin();
    while(lhs != old_entries.end() || rhs != fresh_entries.end()) {
        if(rhs == fresh_entries.end() || (lhs != old_entries.end() && key(*lhs) < key(*rhs))) {
            out.push_back(std::move(*lhs));
            lhs += 1;
        } else if(lhs == old_entries.end() || key(*rhs) < key(*lhs)) {
            out.push_back(std::move(*rhs));
            rhs += 1;
        } else {
            lhs->mask |= rhs->mask;
            out.push_back(std::move(*lhs));
            lhs += 1;
            rhs += 1;
        }
    }
}

/// Combine adjacent equal-key entries of a sorted run into one entry with
/// OR-ed masks.
template <typename Entry, typename Key>
void combine_equal(std::vector<Entry>& entries, Key key) {
    std::uint32_t out = 0;
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        if(out != 0 && key(entries[out - 1]) == key(entries[i])) {
            entries[out - 1].mask |= entries[i].mask;
        } else {
            if(out != i) {
                entries[out] = std::move(entries[i]);
            }
            out += 1;
        }
    }
    entries.resize(out);
}

/// Sort a run and combine its equal-key entries.
template <typename Entry, typename Key>
void canonicalize(std::vector<Entry>& entries, Key key) {
    ranges::sort(entries, {}, key);
    combine_equal(entries, key);
}

/// The symbol table a set of merged entries requires: occurrence targets,
/// relation group keys, and symbol payloads.
template <typename MaskT>
llvm::DenseSet<SymbolHash> referenced_symbols(const MergedEntries<MaskT>& merged) {
    llvm::DenseSet<SymbolHash> referenced;
    for(auto& entry: merged.occurrences) {
        referenced.insert(entry.occurrence.target);
    }
    for(auto& group: merged.relations) {
        referenced.insert(group.symbol);
        for(auto& entry: group.entries) {
            auto& relation = entry.relation;
            if(relation.target != 0 && !RelationKind(relation.kind).carries_extent()) {
                referenced.insert(relation.target);
            }
        }
    }
    return referenced;
}

/// Decode a blob's occurrences into working form; `mask_of` returns the
/// occurrence's mask in the output id space (empty drops the occurrence).
template <typename MaskT, typename MaskOf>
std::vector<OccurrenceEntry<MaskT>> decode_occurrences(const OccurrenceTable& table,
                                                       MaskOf mask_of) {
    std::vector<OccurrenceEntry<MaskT>> entries;
    entries.reserve(table.size());
    for_each_range(table.columns, [&](std::uint32_t entry, LocalSourceRange range) {
        auto mask = mask_of(entry);
        if(!mask_empty(mask)) {
            entries.push_back({table.at(entry, range), std::move(mask)});
        }
        return true;
    });
    return entries;
}

/// Decode one symbol's relation slice of a blob into working form, `cursor`
/// placed at its first relation.
template <typename MaskT, typename MaskOf>
std::vector<RelationEntry<MaskT>> decode_relation_group(const RelationTable& table,
                                                        std::uint32_t begin_entry,
                                                        std::uint32_t end_entry,
                                                        RelationTable::Cursor& cursor,
                                                        MaskOf mask_of) {
    std::vector<RelationEntry<MaskT>> entries;
    entries.reserve(end_entry - begin_entry);
    for(auto entry = begin_entry; entry < end_entry; entry += 1) {
        auto relation = table.decode(entry, cursor);
        auto mask = mask_of(entry);
        if(mask_empty(mask)) {
            continue;
        }
        entries.push_back({relation, std::move(mask)});
    }
    return entries;
}

/// One blob's relation groups in symbol-hash order, decoded lazily by the
/// group merge.
struct StoredGroup {
    SymbolHash hash;
    std::uint32_t begin_entry;
    std::uint32_t end_entry;
};

std::vector<StoredGroup> relation_groups(const RelationTable& table) {
    std::vector<StoredGroup> groups;
    auto& symbol_hashes = table.root.symbol_hashes;
    for(std::uint32_t id = 0; id < symbol_hashes.size(); id += 1) {
        if(table.offsets[id] != table.offsets[id + 1]) {
            groups.push_back({symbol_hashes[id], table.offsets[id], table.offsets[id + 1]});
        }
    }
    return groups;
}

/// The content identity and line table one blob carries, copied verbatim
/// between blobs of the same generation.
struct ContentInfo {
    std::uint64_t hash = 0;
    std::uint32_t size = 0;
    std::vector<std::uint8_t> line_lengths;
    std::vector<std::uint32_t> long_lines;
    std::vector<std::uint32_t> long_line_lengths;
    std::vector<std::uint64_t> crlf_lines;
    std::vector<std::uint32_t> utf8_lines;
    std::string utf8_text;
    std::vector<std::uint32_t> utf8_text_ends;
};

ContentInfo content_info_of(const Root& root) {
    auto copy = [](const auto& column) {
        return std::vector(column.begin(), column.end());
    };
    return {
        .hash = root.content_hash,
        .size = root.content_size,
        .line_lengths = copy(root.line_lengths),
        .long_lines = copy(root.long_lines),
        .long_line_lengths = copy(root.long_line_lengths),
        .crlf_lines = copy(root.crlf_lines),
        .utf8_lines = copy(root.utf8_lines),
        .utf8_text = std::string(root.utf8_text),
        .utf8_text_ends = copy(root.utf8_text_ends),
    };
}

ContentInfo content_info_of(llvm::StringRef content) {
    ContentInfo info;
    info.hash = llvm::xxh3_64bits(content);
    info.size = static_cast<std::uint32_t>(content.size());

    auto starts = lsp::line_starts({content.data(), content.size()});
    info.line_lengths.reserve(starts.size());
    for(std::uint32_t i = 0; i < starts.size(); i += 1) {
        auto last = i + 1 == starts.size();
        auto next = last ? info.size : starts[i + 1];
        auto length = next - starts[i];
        auto crlf = !last && length >= 2 && content[next - 2] == '\r';
        if(crlf) {
            info.crlf_lines.resize(i / 64 + 1);
            info.crlf_lines[i / 64] |= std::uint64_t(1) << (i % 64);
        }
        auto text = content.slice(starts[i], next - (last ? 0 : crlf ? 2 : 1));
        if(!lsp::is_ascii({text.data(), text.size()})) {
            info.utf8_lines.push_back(i);
            info.utf8_text += text;
            info.utf8_text_ends.push_back(static_cast<std::uint32_t>(info.utf8_text.size()));
        }
        if(length >= length_escape) {
            info.line_lengths.push_back(length_escape);
            info.long_lines.push_back(i);
            info.long_line_lengths.push_back(length);
        } else {
            info.line_lengths.push_back(static_cast<std::uint8_t>(length));
        }
    }
    return info;
}

/// Collect one blob's local symbols; symbols the merged entries no longer
/// reference are filtered at emit time.
void collect_locals(const Root& root, llvm::DenseMap<SymbolHash, SymbolInfo>& locals) {
    auto table = LocalTable::of(root);
    auto relations = RelationTable::of(root);
    for(std::uint32_t k = 0; k < table.size(); k += 1) {
        auto identity = table.at(k);
        auto hash = table.hash_of(k);
        auto [it, inserted] = locals.try_emplace(hash, SymbolInfo::from(hash, identity));
        // Variants may see different facts of one symbol (a definition
        // behind `#ifdef`); like the symbol records, the union is kept, and
        // the token kind a defining variant gives.
        if(!inserted) {
            if(relations.defines(table.symbols[k])) {
                it->second.token_class.kind = identity.token_class.kind;
            }
            it->second.flags |= identity.flags;
            it->second.token_class.modifiers |= identity.token_class.modifiers;
        }
    }
}

/// Appends one side's entry masks in the mask encoding the variant count
/// chose for the whole blob.
struct MaskWriter {
    MaskColumns& masks;
    MaskEncoding encoding;

    template <typename MaskT>
    void add(const MaskT& mask) {
        switch(encoding) {
            case MaskEncoding::Single: {
                break;
            }
            case MaskEncoding::U8: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    masks.masks8.push_back(static_cast<std::uint8_t>(mask));
                }
                break;
            }
            case MaskEncoding::U16: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    masks.masks16.push_back(static_cast<std::uint16_t>(mask));
                }
                break;
            }
            case MaskEncoding::U32: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    masks.masks32.push_back(static_cast<std::uint32_t>(mask));
                }
                break;
            }
            case MaskEncoding::U64: {
                if constexpr(std::same_as<MaskT, std::uint64_t>) {
                    masks.masks64.push_back(mask);
                }
                break;
            }
            case MaskEncoding::Roaring: {
                if constexpr(std::same_as<MaskT, Bitmap>) {
                    auto size = mask.getSizeInBytes(true);
                    auto offset = masks.roaring.size();
                    masks.roaring.resize(offset + size);
                    mask.write(reinterpret_cast<char*>(masks.roaring.data() + offset), true);
                    masks.roaring_offsets.push_back(static_cast<std::uint32_t>(offset));
                }
                break;
            }
        }
    }

    /// The roaring encoding's offset column ends with the size of the slices.
    void finish() {
        if(encoding == MaskEncoding::Roaring) {
            masks.roaring_offsets.push_back(static_cast<std::uint32_t>(masks.roaring.size()));
        }
    }
};

/// Encode merged entries, locals and content into canonical blob bytes. The
/// only entry point that writes a ShardBlob: every self-describing choice
/// (range encoding, id width, mask encoding) is made here, from
/// the data, so equal inputs produce equal bytes.
template <typename MaskT>
std::vector<std::byte> emit_blob(const MergedEntries<MaskT>& merged,
                                 const llvm::DenseMap<SymbolHash, SymbolInfo>& locals,
                                 std::vector<VariantHash> variants,
                                 const ContentInfo& content) {
    // The locals the entries name; their parents join the symbol table,
    // which their parent column indexes.
    auto symbols = referenced_symbols(merged);
    llvm::SmallVector<std::pair<SymbolHash, const SymbolInfo*>> kept;
    for(auto& [hash, info]: locals) {
        if(symbols.contains(hash)) {
            kept.emplace_back(hash, &info);
        }
    }
    ranges::sort(kept, {}, [](const auto& entry) { return entry.first; });
    for(auto& entry: kept) {
        if(entry.second->parent != 0) {
            symbols.insert(entry.second->parent);
        }
    }

    ShardBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = content.hash;
    blob.content_size = content.size;
    blob.utf8_lines = content.utf8_lines;
    blob.utf8_text = content.utf8_text;
    blob.utf8_text_ends = content.utf8_text_ends;
    blob.line_lengths = content.line_lengths;
    blob.long_lines = content.long_lines;
    blob.long_line_lengths = content.long_line_lengths;
    blob.crlf_lines = content.crlf_lines;
    blob.variants = std::move(variants);

    blob.symbol_hashes.assign(symbols.begin(), symbols.end());
    ranges::sort(blob.symbol_hashes);
    auto symbol_index = [&](SymbolHash hash) {
        return *sorted_find(llvm::ArrayRef(blob.symbol_hashes), hash);
    };

    std::vector<std::uint32_t> local_ids;
    std::vector<std::uint32_t> name_ends;
    std::vector<std::uint32_t> args_of;
    std::vector<std::uint32_t> args_ends;
    std::vector<std::uint32_t> signatures_of;
    std::vector<std::uint32_t> signature_ends;
    std::vector<std::uint32_t> parents;
    for(std::uint32_t k = 0; k < kept.size(); k += 1) {
        auto& [hash, info] = kept[k];
        local_ids.push_back(symbol_index(hash));
        blob.local_names += info->name;
        name_ends.push_back(static_cast<std::uint32_t>(blob.local_names.size()));
        if(!info->args.empty()) {
            args_of.push_back(k);
            blob.local_args += info->args;
            args_ends.push_back(static_cast<std::uint32_t>(blob.local_args.size()));
        }
        if(!info->signature.empty()) {
            signatures_of.push_back(k);
            blob.local_signatures += info->signature;
            signature_ends.push_back(static_cast<std::uint32_t>(blob.local_signatures.size()));
        }
        parents.push_back(info->parent == 0 ? 0 : symbol_index(info->parent) + 1);
        blob.local_kinds.push_back(info->kind.value());
        blob.local_scopes.push_back(static_cast<std::uint8_t>(info->scope));
        // Whether a unit defines the symbol varies with the unit, which the
        // bytes of a file's variant must not.
        blob.local_flags.push_back(static_cast<std::uint16_t>(info->flags) &
                                   ~static_cast<std::uint16_t>(SymbolFlags::HasDefinition));
        blob.local_token_kinds.push_back(static_cast<std::uint8_t>(info->token_class.kind));
        blob.local_token_modifiers.push_back(info->token_class.modifiers);
    }
    write_ids(blob.local_symbols, local_ids, blob.symbol_hashes.size());
    write_ids(blob.local_name_ends, name_ends, blob.local_names.size() + 1);
    write_ids(blob.local_args_of, args_of, kept.size());
    write_ids(blob.local_args_ends, args_ends, blob.local_args.size() + 1);
    write_ids(blob.local_signatures_of, signatures_of, kept.size());
    write_ids(blob.local_signature_ends, signature_ends, blob.local_signatures.size() + 1);
    write_ids(blob.local_parents, parents, blob.symbol_hashes.size() + 1);

    auto encoding = encoding_of(blob.variants.size());

    RangeWriter occurrences{.side = blob.occurrences, .encoding = RangeEncoding::Delta};
    MaskWriter occurrence_masks{.masks = blob.occurrence_masks, .encoding = encoding};
    auto occurrence_symbols = id_writer(blob.occurrence_symbols, blob.symbol_hashes.size());
    // The first occurrence of each range, which the relations spanning it name.
    llvm::DenseMap<std::uint64_t, std::uint32_t> occurrence_of;
    for(auto& entry: merged.occurrences) {
        auto range = entry.occurrence.range;
        occurrence_of.try_emplace((std::uint64_t(range.begin) << 32) | range.end,
                                  occurrences.count);
        occurrences.add(range);
        occurrence_masks.add(entry.mask);
        occurrence_symbols.add(symbol_index(entry.occurrence.target));
    }
    occurrence_masks.finish();

    // Relation groups follow symbol-table order; a symbol with occurrences
    // only gets an empty slice.
    RangeWriter explicit_ranges{.side = blob.relation_explicit_ranges,
                                .encoding = unsorted_encoding(content.size)};
    MaskWriter relation_masks{.masks = blob.relation_masks, .encoding = encoding};
    std::uint32_t relations = 0;
    std::uint32_t previous_occurrence = 0;
    std::vector<std::uint32_t> relation_offsets;
    relation_offsets.reserve(blob.symbol_hashes.size() + 1);
    std::vector<std::uint32_t> targets;
    std::vector<std::uint32_t> target_ranks;
    std::vector<std::uint32_t> extent_ranks;
    std::vector<std::uint32_t> range_ranks;
    std::vector<std::uint32_t> explicit_ranks;
    auto group = merged.relations.begin();
    for(auto hash: blob.symbol_hashes) {
        relation_offsets.push_back(relations);
        if(group == merged.relations.end() || group->symbol != hash) {
            continue;
        }
        for(auto& entry: group->entries) {
            auto& relation = entry.relation;
            if(relations % rank_block == 0) {
                target_ranks.push_back(static_cast<std::uint32_t>(targets.size()));
                extent_ranks.push_back(
                    static_cast<std::uint32_t>(blob.relation_extent_heads.size()));
                range_ranks.push_back(static_cast<std::uint32_t>(blob.relation_range_codes.size()));
                explicit_ranks.push_back(explicit_ranges.count);
                previous_occurrence = 0;
            }
            auto kind = static_cast<std::uint8_t>(relation.kind);
            if(relation.target != 0) {
                kind |= payload_bit;
                if(RelationKind(relation.kind).carries_extent()) {
                    auto extent = relation.extent();
                    if(fits_extent_columns(relation.range, extent)) {
                        blob.relation_extent_heads.push_back(
                            static_cast<std::uint16_t>(relation.range.begin - extent.begin));
                        blob.relation_extent_lengths.push_back(
                            static_cast<std::uint16_t>(extent.end - extent.begin));
                    } else {
                        blob.relation_far_extents.push_back(
                            static_cast<std::uint32_t>(blob.relation_extent_heads.size()));
                        blob.relation_far_begins.push_back(extent.begin);
                        blob.relation_far_ends.push_back(extent.end);
                        blob.relation_extent_heads.push_back(extent_escape);
                        blob.relation_extent_lengths.push_back(0);
                    }
                } else {
                    targets.push_back(symbol_index(relation.target));
                }
            }
            blob.relation_kinds.push_back(kind);
            auto range = relation.range;
            if(range == LocalSourceRange{}) {
                write_varint(blob.relation_range_codes, range_none);
            } else if(auto found =
                          occurrence_of.find((std::uint64_t(range.begin) << 32) | range.end);
                      found != occurrence_of.end()) {
                auto distance = static_cast<std::int64_t>(found->second) - previous_occurrence;
                auto zigzag = (static_cast<std::uint64_t>(distance) << 1) ^
                              static_cast<std::uint64_t>(distance >> 63);
                write_varint(blob.relation_range_codes, zigzag << 1);
                previous_occurrence = found->second;
            } else {
                write_varint(blob.relation_range_codes, range_explicit);
                explicit_ranges.add(range);
            }
            relation_masks.add(entry.mask);
            relations += 1;
        }
        group += 1;
    }
    relation_offsets.push_back(relations);
    relation_masks.finish();
    write_ids(blob.relation_offsets, relation_offsets, relations + 1);
    write_ids(blob.relation_targets, targets, blob.symbol_hashes.size());
    write_ids(blob.relation_target_ranks, target_ranks, targets.size() + 1);
    write_ids(blob.relation_extent_ranks, extent_ranks, blob.relation_extent_heads.size() + 1);
    write_ids(blob.relation_code_ranks, range_ranks, blob.relation_range_codes.size() + 1);
    write_ids(blob.relation_explicit_ranks, explicit_ranks, explicit_ranges.count + 1);

    return encode(blob);
}

template <typename MaskT>
std::vector<std::byte> write_merged(const Root* old_root,
                                    llvm::ArrayRef<std::int64_t> id_map,
                                    llvm::ArrayRef<Shard> fresh,
                                    std::uint32_t fresh_base,
                                    std::vector<VariantHash> variants,
                                    const ContentInfo& content,
                                    llvm::function_ref<bool(std::uint32_t)> within) {
    MergedEntries<MaskT> merged;

    // Occurrences: concatenate every fresh blob's entries (each marked with
    // its new bit), sort, combine equal entries, then merge with the old
    // entries. Fresh runs are individually sorted already; one sort over the
    // concatenation keeps the merge two-way.
    std::vector<OccurrenceEntry<MaskT>> fresh_occurrences;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto bit = single_bit<MaskT>(fresh_base + i);
        auto entries = decode_occurrences<MaskT>(OccurrenceTable::of(view_of(fresh[i].bytes())),
                                                 [&](std::uint32_t) { return bit; });
        fresh_occurrences.insert(fresh_occurrences.end(),
                                 std::make_move_iterator(entries.begin()),
                                 std::make_move_iterator(entries.end()));
    }
    canonicalize(fresh_occurrences, occurrence_key);

    std::vector<OccurrenceEntry<MaskT>> old_occurrences;
    // With `within`, the symbols the old occurrences it keeps name.
    llvm::DenseSet<SymbolHash> named;
    if(old_root) {
        auto table = OccurrenceTable::of(*old_root);
        auto encoding = encoding_of(*old_root);
        old_occurrences = decode_occurrences<MaskT>(table, [&](std::uint32_t entry) {
            return remap_mask<MaskT>(encoding, table.masks, entry, id_map);
        });
        if(within) {
            std::erase_if(old_occurrences,
                          [&](const auto& entry) { return !within(entry.occurrence.range.begin); });
            for(const auto& entry: old_occurrences) {
                named.insert(entry.occurrence.target);
            }
        }
    }
    merge_sorted(std::move(old_occurrences),
                 std::move(fresh_occurrences),
                 occurrence_key,
                 merged.occurrences);

    // Relations: gather fresh groups per symbol across all fresh blobs,
    // then two-way merge with the old blob's groups in hash order.
    llvm::DenseMap<SymbolHash, std::vector<RelationEntry<MaskT>>> fresh_group_map;
    for(std::uint32_t i = 0; i < fresh.size(); i += 1) {
        auto table = RelationTable::of(view_of(fresh[i].bytes()));
        auto bit = single_bit<MaskT>(fresh_base + i);
        RelationTable::Cursor cursor;
        for(auto& group: relation_groups(table)) {
            auto entries = decode_relation_group<MaskT>(table,
                                                        group.begin_entry,
                                                        group.end_entry,
                                                        cursor,
                                                        [&](std::uint32_t) { return bit; });
            auto& into = fresh_group_map[group.hash];
            into.insert(into.end(),
                        std::make_move_iterator(entries.begin()),
                        std::make_move_iterator(entries.end()));
        }
    }
    std::vector<RelationGroup<MaskT>> fresh_groups;
    fresh_groups.reserve(fresh_group_map.size());
    for(auto& [hash, entries]: fresh_group_map) {
        canonicalize(entries, relation_key);
        fresh_groups.push_back({hash, std::move(entries)});
    }
    ranges::sort(fresh_groups, {}, group_key);

    std::vector<StoredGroup> old_groups;
    std::optional<RelationTable> old_table;
    MaskEncoding old_encoding = MaskEncoding::Single;
    if(old_root) {
        old_table.emplace(RelationTable::of(*old_root));
        old_encoding = encoding_of(*old_root);
        old_groups = relation_groups(*old_table);
    }
    // The merge below decodes every old group once, in order.
    RelationTable::Cursor old_cursor;
    auto decode_old = [&](const StoredGroup& group) {
        auto entries = decode_relation_group<MaskT>(
            *old_table,
            group.begin_entry,
            group.end_entry,
            old_cursor,
            [&](std::uint32_t entry) {
                return remap_mask<MaskT>(old_encoding, old_table->masks, entry, id_map);
            });
        if(within) {
            std::erase_if(entries, [&](const auto& entry) {
                // A relation between two symbols has no place of its own:
                // it goes with the symbols the kept occurrences name.
                if(!entry.relation.range.valid()) {
                    return !named.contains(group.hash) && !named.contains(entry.relation.target);
                }
                return !within(entry.relation.range.begin);
            });
        }
        return entries;
    };

    auto lhs = old_groups.begin();
    auto rhs = fresh_groups.begin();
    while(lhs != old_groups.end() || rhs != fresh_groups.end()) {
        if(rhs == fresh_groups.end() || (lhs != old_groups.end() && lhs->hash < rhs->symbol)) {
            auto entries = decode_old(*lhs);
            if(!entries.empty()) {
                merged.relations.push_back({lhs->hash, std::move(entries)});
            }
            lhs += 1;
        } else if(lhs == old_groups.end() || rhs->symbol < lhs->hash) {
            merged.relations.push_back(std::move(*rhs));
            rhs += 1;
        } else {
            std::vector<RelationEntry<MaskT>> combined;
            merge_sorted(decode_old(*lhs), std::move(rhs->entries), relation_key, combined);
            if(!combined.empty()) {
                merged.relations.push_back({lhs->hash, std::move(combined)});
            }
            lhs += 1;
            rhs += 1;
        }
    }

    // Local names union: every input blob is self-contained, so the merge
    // needs no external symbol resolver. First writer wins — identities of
    // one symbol agree across blobs of one file.
    llvm::DenseMap<SymbolHash, SymbolInfo> locals;
    if(old_root) {
        collect_locals(*old_root, locals);
    }
    for(auto& shard: fresh) {
        collect_locals(view_of(shard.bytes()), locals);
    }

    return emit_blob(merged, locals, std::move(variants), content);
}

}  // namespace

std::vector<std::byte>
    write_shard(const FileIndex& file_index,
                llvm::function_ref<std::optional<SymbolView>(SymbolHash)> symbols,
                llvm::StringRef content) {
    // Canonicalize straight from the in-memory entries: sorted, deduplicated.
    // MaskT is irrelevant for a single variant (no mask columns) — use the
    // cheap one.
    MergedEntries<std::uint64_t> merged;
    merged.occurrences.reserve(file_index.occurrences.size());
    for(auto& occurrence: file_index.occurrences) {
        merged.occurrences.push_back({occurrence, 1});
    }
    canonicalize(merged.occurrences, occurrence_key);

    merged.relations.reserve(file_index.relations.size());
    for(auto& [hash, relations]: file_index.relations) {
        std::vector<RelationEntry<std::uint64_t>> group;
        group.reserve(relations.size());
        for(auto& relation: relations) {
            group.push_back({relation, 1});
        }
        canonicalize(group, relation_key);
        merged.relations.push_back({hash, std::move(group)});
    }
    ranges::sort(merged.relations, {}, group_key);

    llvm::DenseMap<SymbolHash, SymbolInfo> locals;
    if(symbols) {
        for(auto hash: referenced_symbols(merged)) {
            auto found = symbols(hash);
            if(found && found->scope != SymbolScope::External) {
                locals.try_emplace(hash, SymbolInfo::from(hash, *found));
            }
        }
    }

    return emit_blob(merged, locals, {}, content_info_of(content));
}

/// merge_shards, keeping of `old`'s entries those `within` accepts when
/// given.
static std::vector<std::byte> merge_blobs(const Shard& old,
                                          llvm::ArrayRef<VariantHash> keep,
                                          llvm::ArrayRef<Shard> fresh,
                                          llvm::function_ref<bool(std::uint32_t)> within) {
    auto old_variants = old.variants();

    // old-id -> new-id; -1 drops the variant.
    llvm::SmallVector<std::int64_t> id_map(old_variants.size(), -1);
    std::vector<VariantHash> variants;
    for(std::uint32_t id = 0; id < old_variants.size(); id += 1) {
        if(ranges::contains(keep, old_variants[id])) {
            id_map[id] = static_cast<std::int64_t>(variants.size());
            variants.push_back(old_variants[id]);
        }
    }

    const Root* old_root = nullptr;
    if(!variants.empty()) {
        old_root = &view_of(old.bytes());
    }

    auto fresh_base = static_cast<std::uint32_t>(variants.size());
    for(auto& shard: fresh) {
        assert(shard.loaded() && "fresh shards must hold a blob");
        auto identity = shard.variants();
        assert(identity.size() == 1 && "fresh shards are single-variant worker blobs");
        assert(!ranges::contains(variants, identity.front()) &&
               "a variant already stored must not be re-appended");
        variants.push_back(identity.front());
    }
    assert(!variants.empty() && "a shard blob holds at least one variant");

    // Content and line table travel verbatim from any input — all inputs
    // share one content generation. Offsets from different generations
    // must never share one blob's entries, hence the assert.
    ContentInfo content =
        old_root ? content_info_of(*old_root) : content_info_of(view_of(fresh.front().bytes()));
    for([[maybe_unused]] auto& shard: fresh) {
        assert(shard.content_hash() == content.hash &&
               "merge inputs must share one content generation");
    }

    // LIMITATION: a merge decodes and writes every stored variant again to
    // add one, so a header that differs in each unit — the kernel's
    // `READ_ONCE`, whose inline functions use `__COUNTER__` — costs its
    // units' merges the square of their number, on the master's loop.
    if(variants.size() <= 64) {
        return write_merged<std::uint64_t>(old_root,
                                           id_map,
                                           fresh,
                                           fresh_base,
                                           std::move(variants),
                                           content,
                                           within);
    }
    return write_merged<Bitmap>(old_root,
                                id_map,
                                fresh,
                                fresh_base,
                                std::move(variants),
                                content,
                                within);
}

std::vector<std::byte> merge_shards(const Shard& old,
                                    llvm::ArrayRef<VariantHash> keep,
                                    llvm::ArrayRef<Shard> fresh) {
    return merge_blobs(old, keep, fresh, nullptr);
}

/// A shard's entries counted and hashed, whichever variants hold them: a
/// merge only adds to the entries of the shards it takes, so equal counts
/// are equal entries.
static std::pair<std::size_t, std::uint64_t> count_entries(const Shard& shard) {
    std::size_t count = 0;
    std::uint64_t hash = 0;
    shard.for_each_occurrence([&](const Occurrence& occurrence) {
        count += 1;
        hash = llvm::hash_combine(hash,
                                  occurrence.range.begin,
                                  occurrence.range.end,
                                  occurrence.target);
        return true;
    });
    shard.for_each_relation([&](SymbolHash symbol, const Relation& relation) {
        count += 1;
        hash = llvm::hash_combine(hash,
                                  symbol,
                                  static_cast<std::uint32_t>(relation.kind),
                                  relation.range.begin,
                                  relation.range.end,
                                  relation.target);
        return true;
    });
    return {count, hash};
}

std::optional<MergedContexts> merge_contexts(const Shard& stored,
                                             llvm::ArrayRef<VariantHash> live,
                                             const Shard& own,
                                             llvm::ArrayRef<std::uint32_t> skipped) {
    llvm::SmallVector<VariantHash> keep(live.begin(), live.end());
    auto fresh = own.loaded() ? llvm::ArrayRef(&own, 1) : llvm::ArrayRef<Shard>();
    if(own.loaded()) {
        keep.erase(ranges::remove(keep, own.variants().front()).begin(), keep.end());
    } else if(ranges::none_of(keep, [&](VariantHash hash) { return stored.has_variant(hash); })) {
        // The shard outlives the last context reading the file until the
        // index is saved.
        return std::nullopt;
    }
    auto bytes = merge_blobs(stored, keep, fresh, [&](std::uint32_t offset) {
        return inactive_at(skipped, offset);
    });
    MergedContexts merged{
        .shard = Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes))),
    };
    auto [count, hash] = count_entries(merged.shard);
    if(count != count_entries(own).first) {
        merged.additions = hash;
    }
    return merged;
}

}  // namespace clice::index
