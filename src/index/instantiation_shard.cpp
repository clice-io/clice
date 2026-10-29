module clice;

import :index.columns;
import :index.format;
import :index.instantiation_shard;
import :support.bytes;
import :support.logging;

namespace clice::index {

namespace {

using Root = codec::blob::view_t<InstantiationShardBlob>;

const Root& view_of(llvm::StringRef bytes) {
    return codec::blob::view_verified<InstantiationShardBlob>(as_bytes(bytes));
}

/// A record kind's byte: the InstantiationKind, with this bit set for a
/// macro expansion's record.
constexpr std::uint8_t expansion_bit = 0x80;

std::uint8_t kind_byte(const InstantiationRecord& record) {
    return static_cast<std::uint8_t>(record.kind) | (record.expansion ? expansion_bit : 0);
}

/// The record table: each record's range, kind, target and `from`.
struct RecordTable {
    const Root& root;
    const RangeColumnsView& columns;
    IdColumnView targets;
    IdColumnView froms;

    static RecordTable of(const Root& root) {
        auto& symbol_hashes = root.symbol_hashes;
        return {
            .root = root,
            .columns = root.records,
            .targets = ids_of(root.record_targets, symbol_hashes.size()),
            .froms = ids_of(root.record_froms, symbol_hashes.size()),
        };
    }

    std::uint32_t size() const {
        return entry_count(columns);
    }

    InstantiationKind kind_of(std::uint32_t record) const {
        return static_cast<InstantiationKind>(root.record_kinds[record] & ~expansion_bit);
    }

    bool expansion_of(std::uint32_t record) const {
        return (root.record_kinds[record] & expansion_bit) != 0;
    }

    InstantiationRecord at(std::uint32_t record, LocalSourceRange range) const {
        auto& symbol_hashes = root.symbol_hashes;
        auto kind = kind_of(record);
        return {
            .range = range,
            .kind = kind,
            .target = symbol_hashes[targets[record]],
            .from = has_from(kind) ? symbol_hashes[froms[record]] : 0,
            .expansion = expansion_of(record),
        };
    }
};

/// Sets of another table's entries under their identities, ascending — the
/// resolutions, sets of records, and the uses, sets of resolutions: set i's
/// members are members[ends[i - 1], ends[i]), ascending.
struct SetTable {
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::ArrayRef<std::uint32_t> ends;
    IdColumnView members;
    /// The size of the table the members index.
    std::uint32_t member_count;

    static SetTable resolutions_of(const Root& root) {
        auto records = entry_count(root.records);
        return {
            .hashes = root.resolutions,
            .ends = root.resolution_ends,
            .members = ids_of(root.resolution_records, records),
            .member_count = records,
        };
    }

    static SetTable uses_of(const Root& root) {
        auto resolutions = static_cast<std::uint32_t>(root.resolutions.size());
        return {
            .hashes = root.uses,
            .ends = root.use_ends,
            .members = ids_of(root.use_resolutions, resolutions),
            .member_count = resolutions,
        };
    }

    /// The members of set `set`.
    auto members_of(std::uint32_t set) const {
        return views::iota(set == 0 ? 0 : ends[set - 1], ends[set]) |
               views::transform([this](std::uint32_t i) { return members[i]; });
    }

    std::optional<std::uint32_t> find(std::uint64_t hash) const {
        return sorted_find(hashes, hash);
    }
};

struct KeyTable {
    const Root& root;
    IdColumnView patterns;
    IdColumnView resolutions;
    IdColumnView producers;

    static KeyTable of(const Root& root) {
        return {
            .root = root,
            .patterns = ids_of(root.key_patterns, root.symbol_hashes.size()),
            .resolutions = ids_of(root.key_resolutions, root.resolutions.size() + 1),
            .producers = ids_of(root.key_producers, root.producer_units.size()),
        };
    }

    InstantiationShard::Key at(std::uint32_t k) const {
        InstantiationShard::Key key{
            .key = root.keys[k],
            .pattern = root.symbol_hashes[patterns[k]],
        };
        if(auto resolution = resolutions[k]; resolution != 0) {
            key.resolution = root.resolutions[resolution - 1];
        }
        if(producers.size() != 0) {
            auto producer = producers[k];
            key.producer = Producer{
                .unit = root.producer_units[producer],
                .run = root.producer_runs[producer],
            };
        }
        return key;
    }
};

// Decoded symbol hashes, like resolution hashes and producer units, key
// DenseMaps (reserved_key).
CheckResult symbols_ok(const Root& root) {
    auto& hashes = root.symbol_hashes;
    if(!strictly_ascending(hashes)) {
        return std::unexpected("symbol hashes are not strictly ascending");
    }
    if(ranges::any_of(hashes, [](std::uint64_t hash) { return reserved_key(hash); })) {
        return std::unexpected("a reserved symbol hash");
    }
    return {};
}

// records_at narrows its scan by begin order and the merge decodes
// records by their key.
CheckResult records_ok(const Root& root) {
    auto table = RecordTable::of(root);
    if(auto ok = range_encoding_ok(table.columns, root.content_size, RangeEncoding::Delta); !ok) {
        return ok;
    }
    auto count = table.size();
    if(root.record_kinds.size() != count) {
        return std::unexpected("record kinds do not match the record count");
    }
    auto& symbol_hashes = root.symbol_hashes;
    if(auto ok = ids_ok(table.targets, count, symbol_hashes.size()); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.froms, count, symbol_hashes.size()); !ok) {
        return ok;
    }
    std::tuple<std::uint32_t, std::uint32_t, InstantiationKind, bool, std::uint32_t, std::uint32_t>
        previous;
    CheckResult result;
    for_each_range(table.columns, [&](std::uint32_t record, LocalSourceRange range) {
        if(table.kind_of(record) > InstantiationKind::TypeDefinition) {
            result = std::unexpected("unknown record kind");
            return false;
        }
        if(!has_from(table.kind_of(record)) && table.froms[record] != 0) {
            result = std::unexpected("a `from` on a record kind without one");
            return false;
        }
        std::tuple key{range.begin,
                       range.end,
                       table.kind_of(record),
                       table.expansion_of(record),
                       table.targets[record],
                       table.froms[record]};
        if(record != 0 && key <= previous) {
            result = std::unexpected("records are not strictly ascending");
            return false;
        }
        previous = key;
        return true;
    });
    return result;
}

// Identities key DenseMaps once decoded and are found by binary search; an
// empty set is a key's "nothing here" in disguise.
CheckResult sets_ok(const SetTable& table) {
    if(!strictly_ascending(table.hashes) ||
       ranges::any_of(table.hashes, [](std::uint64_t hash) { return reserved_key(hash); })) {
        return std::unexpected("set identities are not strictly ascending");
    }
    if(table.ends.size() != table.hashes.size() ||
       !covering_ends(table.ends, table.members.size())) {
        return std::unexpected("set member lists are not a partition");
    }
    if(auto ok = ids_ok(table.members, table.members.size(), table.member_count); !ok) {
        return ok;
    }
    for(std::uint32_t set = 0; set < table.hashes.size(); set += 1) {
        auto members = table.members_of(set);
        if(members.empty() || !strictly_ascending(members)) {
            return std::unexpected("a set's members are not strictly ascending");
        }
    }
    return {};
}

// Every record has a resolution: an ownerless record would serve until the
// first dead resolution and then vanish.
CheckResult resolutions_ok(const Root& root) {
    auto table = SetTable::resolutions_of(root);
    if(auto ok = sets_ok(table); !ok) {
        return ok;
    }
    std::vector<bool> owned(table.member_count);
    for(std::uint32_t i = 0; i < table.members.size(); i += 1) {
        owned[table.members[i]] = true;
    }
    if(ranges::contains(owned, false)) {
        return std::unexpected("a record no resolution holds");
    }
    return {};
}

// Manifests name a use by its identity, so each must hash to it.
CheckResult uses_ok(const Root& root) {
    auto table = SetTable::uses_of(root);
    if(auto ok = sets_ok(table); !ok) {
        return ok;
    }
    auto& resolutions = root.resolutions;
    std::vector<ResolutionHash> members;
    for(std::uint32_t use = 0; use < table.hashes.size(); use += 1) {
        members.clear();
        for(auto resolution: table.members_of(use)) {
            members.push_back(resolutions[resolution]);
        }
        if(use_hash(members) != table.hashes[use]) {
            return std::unexpected("a use does not hash to its identity");
        }
    }
    return {};
}

CheckResult keys_ok(const Root& root) {
    auto table = KeyTable::of(root);
    auto& keys = root.keys;
    auto count = keys.size();
    if(!strictly_ascending(keys)) {
        return std::unexpected("keys are not strictly ascending");
    }
    if(auto ok = ids_ok(table.patterns, count, root.symbol_hashes.size()); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.resolutions, count, root.resolutions.size() + 1); !ok) {
        return ok;
    }
    auto& producer_units = root.producer_units;
    auto& producer_runs = root.producer_runs;
    auto producers = producer_units.size();
    if(producer_runs.size() != producers) {
        return std::unexpected("producer columns do not line up");
    }
    if(ranges::any_of(producer_units, [](std::uint64_t unit) { return reserved_key(unit); })) {
        return std::unexpected("a reserved producer unit");
    }
    for(std::uint32_t i = 1; i < producers; i += 1) {
        if(std::pair(producer_units[i - 1], producer_runs[i - 1]) >=
           std::pair(producer_units[i], producer_runs[i])) {
            return std::unexpected("producers are not strictly ascending");
        }
    }
    // A worker's blob names no producer; a stored one names one per key.
    if(producers == 0 ? table.producers.size() != 0
                      : !ids_ok(table.producers, count, producers).has_value()) {
        return std::unexpected("key producers do not line up");
    }
    return {};
}

/// Structural verification does not constrain field values: everything the
/// readers dereference through raw column pointers or search by order is
/// proven here, once. Later checks decode through columns earlier ones
/// bounded.
CheckResult validate(const Root& root) {
    constexpr std::array checks = {symbols_ok, records_ok, resolutions_ok, uses_ok, keys_ok};
    for(auto check: checks) {
        if(auto ok = check(root); !ok) {
            return ok;
        }
    }
    return {};
}

}  // namespace

ResolutionHash resolution_hash(std::uint64_t content_hash,
                               llvm::ArrayRef<InstantiationRecord> records) {
    llvm::SmallVector<std::uint8_t, 256> bytes;
    auto append = [&](auto value) {
        auto* data = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.append(data, data + sizeof(value));
    };
    append(content_hash);
    for(auto& record: records) {
        append(record.range.begin);
        append(record.range.end);
        append(kind_byte(record));
        append(record.target);
        append(record.from);
    }
    return llvm::xxh3_64bits(bytes);
}

UseHash use_hash(llvm::ArrayRef<ResolutionHash> resolutions) {
    return llvm::xxh3_64bits(
        llvm::ArrayRef(reinterpret_cast<const std::uint8_t*>(resolutions.data()),
                       resolutions.size() * sizeof(ResolutionHash)));
}

InstantiationShard InstantiationShard::from_bytes(llvm::StringRef data) {
    return from_buffer(llvm::MemoryBuffer::getMemBuffer(data, "", false));
}

InstantiationShard InstantiationShard::from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!buffer) {
        return {};
    }
    buffer = in_place_buffer(std::move(buffer));
    auto verified = verify_root<InstantiationShardBlob>(buffer->getBuffer(), "instantiation blob");
    if(!verified) {
        return {};
    }
    auto& root = *verified;
    if(auto ok = validate(root); !ok) {
        LOG_DEBUG("Rejecting instantiation blob: {}", ok.error());
        return {};
    }
    InstantiationShard shard(std::move(buffer));
    for_each_range(RecordTable::of(root).columns, [&](std::uint32_t, LocalSourceRange range) {
        shard.longest = std::max(shard.longest, range.length());
        return true;
    });
    return shard;
}

bool InstantiationShard::rebind(std::unique_ptr<llvm::MemoryBuffer> replacement) {
    return rebind_buffer(buffer, std::move(replacement));
}

std::uint64_t InstantiationShard::content_hash() const {
    return buffer ? view_of(bytes()).content_hash : 0;
}

llvm::ArrayRef<ResolutionHash> InstantiationShard::resolutions() const {
    if(!buffer) {
        return {};
    }
    return view_of(bytes()).resolutions;
}

llvm::ArrayRef<UseHash> InstantiationShard::uses() const {
    if(!buffer) {
        return {};
    }
    return view_of(bytes()).uses;
}

bool InstantiationShard::has_use(UseHash use) const {
    return ranges::binary_search(uses(), use);
}

std::vector<ResolutionHash> InstantiationShard::resolutions_of(UseHash use) const {
    std::vector<ResolutionHash> resolutions;
    if(!buffer) {
        return resolutions;
    }
    auto& root = view_of(bytes());
    auto table = SetTable::uses_of(root);
    auto& hashes = root.resolutions;
    if(auto found = table.find(use)) {
        for(auto resolution: table.members_of(*found)) {
            resolutions.push_back(hashes[resolution]);
        }
    }
    return resolutions;
}

void InstantiationShard::set_live(llvm::ArrayRef<UseHash> live) {
    live_records.clear();
    if(!buffer) {
        return;
    }
    llvm::SmallVector<UseHash> sorted(live);
    ranges::sort(sorted);
    auto& root = view_of(bytes());
    auto uses = SetTable::uses_of(root);
    auto resolutions = SetTable::resolutions_of(root);
    bool dead = false;
    std::vector<bool> live_resolutions(resolutions.hashes.size());
    for(std::uint32_t use = 0; use < uses.hashes.size(); use += 1) {
        if(!ranges::binary_search(sorted, uses.hashes[use])) {
            dead = true;
            continue;
        }
        for(auto resolution: uses.members_of(use)) {
            live_resolutions[resolution] = true;
        }
    }
    std::vector<std::uint64_t> bits((resolutions.member_count + 63) / 64);
    for(std::uint32_t resolution = 0; resolution < resolutions.hashes.size(); resolution += 1) {
        if(!live_resolutions[resolution]) {
            dead = true;
            continue;
        }
        for(auto record: resolutions.members_of(resolution)) {
            bits[record / 64] |= std::uint64_t(1) << (record % 64);
        }
    }
    if(dead) {
        live_records = std::move(bits);
    }
}

bool InstantiationShard::has_dead_uses() const {
    return !live_records.empty();
}

llvm::ArrayRef<SymbolHash> InstantiationShard::keys() const {
    if(!buffer) {
        return {};
    }
    return view_of(bytes()).keys;
}

std::optional<InstantiationShard::Key> InstantiationShard::find_key(SymbolHash key) const {
    if(!buffer) {
        return std::nullopt;
    }
    auto& root = view_of(bytes());
    auto table = KeyTable::of(root);
    auto position = sorted_find(root.keys, key);
    if(!position) {
        return std::nullopt;
    }
    return table.at(*position);
}

void InstantiationShard::for_each_key(llvm::function_ref<bool(const Key&)> callback) const {
    if(!buffer) {
        return;
    }
    auto& root = view_of(bytes());
    auto table = KeyTable::of(root);
    auto count = root.keys.size();
    for(std::uint32_t k = 0; k < count; k += 1) {
        if(!callback(table.at(k))) {
            return;
        }
    }
}

void InstantiationShard::records_at(
    std::uint32_t offset,
    llvm::function_ref<bool(const InstantiationRecord&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RecordTable::of(view_of(bytes()));
    // Records nest (a base specifier around the names in it), so the end
    // column is not monotonic: scan the records beginning close enough
    // before the offset for the longest of them to reach it.
    auto reach = offset - std::min(offset, longest);
    auto first =
        partition_point(table.columns, [&](LocalSourceRange range) { return range.begin < reach; });
    for_each_range(table.columns, first, [&](std::uint32_t record, LocalSourceRange range) {
        if(range.begin > offset) {
            return false;
        }
        return !live(record) || !range.touches(offset) || callback(table.at(record, range));
    });
}

void InstantiationShard::records_of(
    SymbolHash symbol,
    llvm::function_ref<bool(const InstantiationRecord&)> callback) const {
    if(!buffer) {
        return;
    }
    auto& root = view_of(bytes());
    auto table = RecordTable::of(root);
    auto found = sorted_find(root.symbol_hashes, symbol);
    if(!found) {
        return;
    }
    auto id = *found;
    for_each_range(table.columns, [&](std::uint32_t record, LocalSourceRange range) {
        bool names = table.targets[record] == id ||
                     (has_from(table.kind_of(record)) && table.froms[record] == id);
        return !names || !live(record) || callback(table.at(record, range));
    });
}

void InstantiationShard::for_each_record(
    llvm::function_ref<bool(const InstantiationRecord&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RecordTable::of(view_of(bytes()));
    for_each_range(table.columns, [&](std::uint32_t record, LocalSourceRange range) {
        return !live(record) || callback(table.at(record, range));
    });
}

namespace {

/// Everything a write accumulates before choosing column encodings.
struct Draft {
    std::uint64_t content_hash = 0;
    std::uint32_t content_size = 0;
    /// Ascending by identity, each resolution's records sorted and unique.
    std::vector<std::pair<ResolutionHash, std::vector<InstantiationRecord>>> resolutions;
    /// Ascending by key.
    std::vector<InstantiationShard::Key> keys;
    /// Ascending by identity, each use's resolutions stored above.
    std::vector<std::pair<UseHash, std::vector<ResolutionHash>>> uses;
};

/// The single writer of InstantiationShardBlob: every encoding choice is a
/// function of the data.
std::vector<std::byte> emit(const Draft& draft) {
    // The records of every resolution, each stored once.
    std::vector<std::pair<InstantiationRecord, std::uint32_t>> held;
    for(std::uint32_t resolution = 0; resolution < draft.resolutions.size(); resolution += 1) {
        for(auto& record: draft.resolutions[resolution].second) {
            held.emplace_back(record, resolution);
        }
    }
    ranges::sort(held, {}, [](const auto& entry) {
        return std::tuple_cat(record_key(entry.first), std::tuple(entry.second));
    });

    llvm::DenseSet<SymbolHash> referenced;
    for(auto& [record, resolution]: held) {
        referenced.insert(record.target);
        if(has_from(record.kind)) {
            referenced.insert(record.from);
        }
    }
    for(auto& key: draft.keys) {
        referenced.insert(key.pattern);
    }

    InstantiationShardBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = draft.content_hash;
    blob.content_size = draft.content_size;
    blob.symbol_hashes.assign(referenced.begin(), referenced.end());
    ranges::sort(blob.symbol_hashes);
    auto symbol_index = [&](SymbolHash hash) {
        return *sorted_find(llvm::ArrayRef(blob.symbol_hashes), hash);
    };

    RangeWriter ranges{.side = blob.records, .encoding = RangeEncoding::Delta};
    auto targets = id_writer(blob.record_targets, blob.symbol_hashes.size());
    auto froms = id_writer(blob.record_froms, blob.symbol_hashes.size());
    std::vector<std::vector<std::uint32_t>> members(draft.resolutions.size());
    for(std::uint32_t i = 0; i < held.size(); i += 1) {
        auto& [record, resolution] = held[i];
        if(i == 0 || held[i - 1].first != record) {
            ranges.add(record.range);
            blob.record_kinds.push_back(kind_byte(record));
            targets.add(symbol_index(record.target));
            froms.add(has_from(record.kind) ? symbol_index(record.from) : 0);
        }
        members[resolution].push_back(ranges.count - 1);
    }

    auto record_ids = id_writer(blob.resolution_records, ranges.count);
    for(std::uint32_t resolution = 0; resolution < draft.resolutions.size(); resolution += 1) {
        blob.resolutions.push_back(draft.resolutions[resolution].first);
        for(auto record: members[resolution]) {
            record_ids.add(record);
        }
        blob.resolution_ends.push_back(static_cast<std::uint32_t>(record_ids.size()));
    }

    std::vector<std::uint32_t> use_resolutions;
    for(auto& [use, resolutions]: draft.uses) {
        blob.uses.push_back(use);
        for(auto resolution: resolutions) {
            auto position = sorted_find(llvm::ArrayRef(blob.resolutions), resolution);
            assert(position && "a use's resolutions are stored");
            use_resolutions.push_back(*position);
        }
        blob.use_ends.push_back(static_cast<std::uint32_t>(use_resolutions.size()));
    }
    write_ids(blob.use_resolutions, use_resolutions, blob.resolutions.size());

    std::vector<Producer> producers;
    for(auto& key: draft.keys) {
        if(key.producer) {
            producers.push_back(*key.producer);
        }
    }
    auto producer_order = [](const Producer& producer) {
        return std::pair(producer.unit, producer.run);
    };
    ranges::sort(producers, {}, producer_order);
    producers.erase(ranges::unique(producers).begin(), producers.end());
    for(auto& producer: producers) {
        blob.producer_units.push_back(producer.unit);
        blob.producer_runs.push_back(producer.run);
    }

    auto patterns = id_writer(blob.key_patterns, blob.symbol_hashes.size());
    auto resolutions = id_writer(blob.key_resolutions, blob.resolutions.size() + 1);
    auto key_producers = id_writer(blob.key_producers, producers.size());
    for(auto& key: draft.keys) {
        blob.keys.push_back(key.key);
        patterns.add(symbol_index(key.pattern));
        std::uint32_t resolution = 0;
        if(key.resolution) {
            auto position = sorted_find(llvm::ArrayRef(blob.resolutions), *key.resolution);
            assert(position && "a key's resolution is stored");
            resolution = *position + 1;
        }
        resolutions.add(resolution);
        if(key.producer) {
            key_producers.add(static_cast<std::uint32_t>(
                ranges::lower_bound(producers, producer_order(*key.producer), {}, producer_order) -
                producers.begin()));
        }
    }

    return encode(blob);
}

using DecodedResolutions = std::vector<std::pair<ResolutionHash, std::vector<InstantiationRecord>>>;

/// The resolutions of a loaded blob `keep` accepts, with their records, in
/// stored order.
void decode_resolutions(const InstantiationShard& shard,
                        llvm::function_ref<bool(ResolutionHash)> keep,
                        DecodedResolutions& out) {
    auto& root = view_of(shard.bytes());
    auto records = RecordTable::of(root);
    std::vector<LocalSourceRange> ranges;
    ranges.reserve(records.size());
    for_each_range(records.columns, [&](std::uint32_t, LocalSourceRange range) {
        ranges.push_back(range);
        return true;
    });
    auto table = SetTable::resolutions_of(root);
    for(std::uint32_t resolution = 0; resolution < table.hashes.size(); resolution += 1) {
        if(!keep(table.hashes[resolution])) {
            continue;
        }
        out.push_back({table.hashes[resolution], {}});
        auto& list = out.back().second;
        for(auto record: table.members_of(resolution)) {
            list.push_back(records.at(record, ranges[record]));
        }
    }
}

/// Order resolutions by identity and fold the ones stored twice.
void fold_resolutions(DecodedResolutions& resolutions) {
    auto identity = [](const auto& resolution) {
        return resolution.first;
    };
    ranges::stable_sort(resolutions, {}, identity);
    resolutions.erase(ranges::unique(resolutions, {}, identity).begin(), resolutions.end());
}

}  // namespace

std::vector<std::byte> write_instantiations(const FileInstantiations& instantiations,
                                            std::uint64_t content_hash,
                                            std::uint32_t content_size) {
    Draft draft{.content_hash = content_hash, .content_size = content_size};
    std::vector<ResolutionHash> identities;
    for(auto records: instantiations.resolutions) {
        ranges::sort(records, {}, record_key);
        records.erase(ranges::unique(records).begin(), records.end());
        identities.push_back(resolution_hash(content_hash, records));
        draft.resolutions.emplace_back(identities.back(), std::move(records));
    }
    fold_resolutions(draft.resolutions);

    for(auto& key: instantiations.keys) {
        auto& added = draft.keys.emplace_back();
        added.key = key.key;
        added.pattern = key.pattern;
        if(key.resolution) {
            added.resolution = identities[*key.resolution];
        }
    }
    ranges::sort(draft.keys, {}, &InstantiationShard::Key::key);
    draft.keys.erase(ranges::unique(draft.keys, {}, &InstantiationShard::Key::key).begin(),
                     draft.keys.end());
    return emit(draft);
}

std::optional<std::vector<std::byte>>
    merge_instantiations(const InstantiationShard& old,
                         llvm::ArrayRef<UseHash> keep,
                         llvm::function_ref<bool(const Producer&)> keep_producer,
                         const InstantiationShard* fresh,
                         Producer producer,
                         std::optional<AddedUse> added) {
    assert((!fresh || !old.loaded() || old.content_hash() == fresh->content_hash()) &&
           "merge inputs must share one content generation");
    auto& root = view_of((fresh ? *fresh : old).bytes());
    Draft draft{
        .content_hash = root.content_hash,
        .content_size = root.content_size,
    };

    // The uses kept, and the added one; their resolutions are the ones of
    // `old` that survive.
    llvm::SmallVector<UseHash> kept_uses(keep);
    ranges::sort(kept_uses);
    for(auto use: old.uses()) {
        if(ranges::binary_search(kept_uses, use)) {
            draft.uses.emplace_back(use, old.resolutions_of(use));
        }
    }
    if(added) {
        draft.uses.emplace_back(added->use,
                                std::vector(added->resolutions.begin(), added->resolutions.end()));
    }
    auto use_order = [](const auto& use) {
        return use.first;
    };
    ranges::sort(draft.uses, {}, use_order);
    draft.uses.erase(ranges::unique(draft.uses, {}, use_order).begin(), draft.uses.end());
    llvm::DenseSet<ResolutionHash> used;
    for(auto& resolutions: views::values(draft.uses)) {
        used.insert(resolutions.begin(), resolutions.end());
    }

    if(old.loaded()) {
        decode_resolutions(
            old,
            [&](ResolutionHash resolution) { return used.contains(resolution); },
            draft.resolutions);
    }
    if(fresh) {
        decode_resolutions(*fresh, [](ResolutionHash) { return true; }, draft.resolutions);
    }
    fold_resolutions(draft.resolutions);
    auto stored = [&](const std::optional<ResolutionHash>& resolution) {
        return !resolution ||
               ranges::binary_search(draft.resolutions, *resolution, {}, [](const auto& entry) {
                   return entry.first;
               });
    };

    std::vector<InstantiationShard::Key> walked;
    if(fresh) {
        fresh->for_each_key([&](const InstantiationShard::Key& key) {
            walked.push_back({.key = key.key,
                              .pattern = key.pattern,
                              .resolution = key.resolution,
                              .producer = producer});
            return true;
        });
    }
    std::vector<InstantiationShard::Key> kept;
    old.for_each_key([&](const InstantiationShard::Key& key) {
        if(keep_producer(*key.producer) && stored(key.resolution) &&
           !ranges::binary_search(walked, key.key, {}, &InstantiationShard::Key::key)) {
            kept.push_back({.key = key.key,
                            .pattern = key.pattern,
                            .resolution = key.resolution,
                            .producer = key.producer});
        }
        return true;
    });
    ranges::merge(kept,
                  walked,
                  std::back_inserter(draft.keys),
                  {},
                  &InstantiationShard::Key::key,
                  &InstantiationShard::Key::key);
    if(draft.resolutions.empty() && draft.keys.empty()) {
        return std::nullopt;
    }
    return emit(draft);
}

}  // namespace clice::index
