module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.instantiation_shard;
import :index.serialization;
import :support.logging;

namespace clice::index {

namespace {

using BlobView = kota::codec::fbs::table_view<InstantiationBlob>;

/// The blob was fully verified at load; per-query views skip that cost.
BlobView view_of(llvm::StringRef bytes) {
    return BlobView::from_verified_bytes(blob_bytes(bytes));
}

/// A record kind's byte: the InstantiationKind, with this bit set for a
/// macro expansion's record.
constexpr std::uint8_t expansion_bit = 0x80;

std::uint8_t kind_byte(const InstantiationRecord& record) {
    return static_cast<std::uint8_t>(record.kind) | (record.expansion ? expansion_bit : 0);
}

/// The record table: each record's range, kind, target and `from`.
struct RecordTable {
    RangeColumnsView columns;
    llvm::ArrayRef<std::uint8_t> kinds;
    IdColumnView targets;
    IdColumnView froms;
    llvm::ArrayRef<std::uint64_t> sym_hashes;

    static RecordTable of(BlobView root) {
        auto sym_hashes = to_array_ref(root[&InstantiationBlob::sym_hashes]);
        return {
            .columns = columns_of(root[&InstantiationBlob::records]),
            .kinds = to_array_ref(root[&InstantiationBlob::record_kinds]),
            .targets = ids_of(root[&InstantiationBlob::record_targets], sym_hashes.size()),
            .froms = ids_of(root[&InstantiationBlob::record_froms], sym_hashes.size()),
            .sym_hashes = sym_hashes,
        };
    }

    std::size_t size() const {
        return columns.size();
    }

    InstantiationKind kind_of(std::uint32_t record) const {
        return static_cast<InstantiationKind>(kinds[record] & ~expansion_bit);
    }

    bool expansion_of(std::uint32_t record) const {
        return (kinds[record] & expansion_bit) != 0;
    }

    InstantiationRecord at(std::uint32_t record) const {
        auto kind = kind_of(record);
        return {
            .range = columns.range_of(record),
            .kind = kind,
            .target = sym_hashes[targets[record]],
            .from = has_from(kind) ? sym_hashes[froms[record]] : 0,
            .expansion = expansion_of(record),
        };
    }
};

/// The resolution table: resolution i's records are records[ends[i - 1], ends[i]).
struct ResolutionTable {
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::ArrayRef<std::uint32_t> ends;
    IdColumnView records;

    static ResolutionTable of(BlobView root) {
        return {
            .hashes = to_array_ref(root[&InstantiationBlob::resolutions]),
            .ends = to_array_ref(root[&InstantiationBlob::resolution_ends]),
            .records = ids_of(root[&InstantiationBlob::resolution_records],
                              columns_of(root[&InstantiationBlob::records]).size()),
        };
    }

    std::uint32_t begin_of(std::uint32_t resolution) const {
        return resolution == 0 ? 0 : ends[resolution - 1];
    }
};

/// The use table: use i's resolutions are resolutions[ends[i - 1], ends[i]).
struct UseTable {
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::ArrayRef<std::uint32_t> ends;
    IdColumnView resolutions;

    static UseTable of(BlobView root) {
        return {
            .hashes = to_array_ref(root[&InstantiationBlob::uses]),
            .ends = to_array_ref(root[&InstantiationBlob::use_ends]),
            .resolutions = ids_of(root[&InstantiationBlob::use_resolutions],
                                  to_array_ref(root[&InstantiationBlob::resolutions]).size()),
        };
    }

    std::uint32_t begin_of(std::uint32_t use) const {
        return use == 0 ? 0 : ends[use - 1];
    }

    std::optional<std::uint32_t> find(UseHash use) const {
        auto it = std::ranges::lower_bound(hashes, use);
        if(it == hashes.end() || *it != use) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - hashes.begin());
    }
};

struct KeyTable {
    llvm::ArrayRef<std::uint64_t> keys;
    IdColumnView patterns;
    IdColumnView resolutions;
    IdColumnView producers;
    llvm::ArrayRef<std::uint64_t> sym_hashes;
    llvm::ArrayRef<std::uint64_t> resolution_hashes;
    llvm::ArrayRef<std::uint64_t> producer_units;
    llvm::ArrayRef<std::uint64_t> producer_runs;

    static KeyTable of(BlobView root) {
        auto sym_hashes = to_array_ref(root[&InstantiationBlob::sym_hashes]);
        auto resolution_hashes = to_array_ref(root[&InstantiationBlob::resolutions]);
        auto producer_units = to_array_ref(root[&InstantiationBlob::producer_units]);
        return {
            .keys = to_array_ref(root[&InstantiationBlob::keys]),
            .patterns = ids_of(root[&InstantiationBlob::key_patterns], sym_hashes.size()),
            .resolutions =
                ids_of(root[&InstantiationBlob::key_resolutions], resolution_hashes.size() + 1),
            .producers = ids_of(root[&InstantiationBlob::key_producers], producer_units.size()),
            .sym_hashes = sym_hashes,
            .resolution_hashes = resolution_hashes,
            .producer_units = producer_units,
            .producer_runs = to_array_ref(root[&InstantiationBlob::producer_runs]),
        };
    }

    InstantiationShard::Key at(std::uint32_t k) const {
        InstantiationShard::Key key{
            .key = keys[k],
            .pattern = sym_hashes[patterns[k]],
        };
        if(auto resolution = resolutions[k]; resolution != 0) {
            key.resolution = resolution_hashes[resolution - 1];
        }
        if(producers.size() != 0) {
            auto producer = producers[k];
            key.producer =
                Producer{.unit = producer_units[producer], .run = producer_runs[producer]};
        }
        return key;
    }
};

// Decoded symbol hashes, like resolution hashes and producer units, key
// DenseMaps (reserved_key).
CheckResult symbols_ok(BlobView root) {
    auto hashes = to_array_ref(root[&InstantiationBlob::sym_hashes]);
    if(!std::ranges::is_sorted(hashes, std::less_equal{})) {
        return std::unexpected("symbol hashes are not strictly ascending");
    }
    if(llvm::any_of(hashes, [](std::uint64_t hash) { return reserved_key(hash); })) {
        return std::unexpected("a reserved symbol hash");
    }
    return {};
}

// lookup(offset) narrows its scan by begin order, the merge decodes records
// by their key, and every range is served as a source range into the
// file's text.
CheckResult records_ok(BlobView root) {
    auto table = RecordTable::of(root);
    auto content_size = root[&InstantiationBlob::content_size];
    if(auto ok = range_encoding_ok(table.columns, content_size, true); !ok) {
        return ok;
    }
    if(!table.columns.masks8.empty() || !table.columns.masks16.empty() ||
       !table.columns.masks32.empty() || !table.columns.masks64.empty() ||
       !table.columns.roaring_offsets.empty() || !table.columns.roaring.empty()) {
        return std::unexpected("mask columns on instantiation records");
    }
    auto count = table.size();
    if(table.kinds.size() != count) {
        return std::unexpected("record kinds do not match the record count");
    }
    if(auto ok = ids_ok(table.targets, count, table.sym_hashes.size()); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.froms, count, table.sym_hashes.size()); !ok) {
        return ok;
    }
    std::tuple<std::uint32_t, std::uint32_t, InstantiationKind, bool, std::uint32_t, std::uint32_t>
        previous;
    for(std::uint32_t record = 0; record < count; record += 1) {
        if(table.kind_of(record) > InstantiationKind::TypeDefinition) {
            return std::unexpected("unknown record kind");
        }
        if(!has_from(table.kind_of(record)) && table.froms[record] != 0) {
            return std::unexpected("a `from` on a record kind without one");
        }
        auto begin = table.columns.begin_of(record);
        auto end = table.columns.end_of(record);
        if(is_no_range(begin, end) || end < begin || end > content_size) {
            return std::unexpected("record range past the content");
        }
        std::tuple key{begin,
                       end,
                       table.kind_of(record),
                       table.expansion_of(record),
                       table.targets[record],
                       table.froms[record]};
        if(record != 0 && key <= previous) {
            return std::unexpected("records are not strictly ascending");
        }
        previous = key;
    }
    return {};
}

// Every resolution holds records and every record a resolution: an ownerless
// record would serve until the first dead resolution and then vanish, and an
// empty resolution is a key's "nothing here" in disguise.
CheckResult resolutions_ok(BlobView root) {
    auto table = ResolutionTable::of(root);
    auto record_count = RecordTable::of(root).size();
    if(!std::ranges::is_sorted(table.hashes, std::less_equal{})) {
        return std::unexpected("resolutions are not strictly ascending");
    }
    if(llvm::any_of(table.hashes, [](std::uint64_t hash) { return reserved_key(hash); })) {
        return std::unexpected("a reserved resolution hash");
    }
    if(table.ends.size() != table.hashes.size() ||
       !covering_ends(table.ends, table.records.size())) {
        return std::unexpected("resolution record lists are not a partition");
    }
    if(auto ok = ids_ok(table.records, table.records.size(), record_count); !ok) {
        return ok;
    }
    std::vector<bool> owned(record_count);
    for(std::uint32_t resolution = 0; resolution < table.hashes.size(); resolution += 1) {
        auto begin = table.begin_of(resolution);
        auto end = table.ends[resolution];
        if(begin == end) {
            return std::unexpected("a resolution without records");
        }
        for(auto i = begin; i < end; i += 1) {
            if(i != begin && table.records[i] <= table.records[i - 1]) {
                return std::unexpected("a resolution's records are not strictly ascending");
            }
            owned[table.records[i]] = true;
        }
    }
    if(llvm::is_contained(owned, false)) {
        return std::unexpected("a record no resolution holds");
    }
    return {};
}

// Manifests name a use by its identity and liveness follows its resolutions,
// so each lists stored resolutions, ascending, under the identity they hash
// to.
CheckResult uses_ok(BlobView root) {
    auto table = UseTable::of(root);
    auto resolutions = to_array_ref(root[&InstantiationBlob::resolutions]);
    if(!std::ranges::is_sorted(table.hashes, std::less_equal{}) ||
       llvm::any_of(table.hashes, [](std::uint64_t hash) { return reserved_key(hash); })) {
        return std::unexpected("uses are not strictly ascending");
    }
    if(table.ends.size() != table.hashes.size() ||
       !covering_ends(table.ends, table.resolutions.size())) {
        return std::unexpected("use resolution lists are not a partition");
    }
    if(auto ok = ids_ok(table.resolutions, table.resolutions.size(), resolutions.size()); !ok) {
        return ok;
    }
    std::vector<ResolutionHash> members;
    for(std::uint32_t use = 0; use < table.hashes.size(); use += 1) {
        members.clear();
        for(auto i = table.begin_of(use); i < table.ends[use]; i += 1) {
            if(i != table.begin_of(use) && table.resolutions[i] <= table.resolutions[i - 1]) {
                return std::unexpected("a use's resolutions are not strictly ascending");
            }
            members.push_back(resolutions[table.resolutions[i]]);
        }
        if(members.empty() || use_identity(members) != table.hashes[use]) {
            return std::unexpected("a use does not hash to its identity");
        }
    }
    return {};
}

CheckResult keys_ok(BlobView root) {
    auto table = KeyTable::of(root);
    auto count = table.keys.size();
    if(!std::ranges::is_sorted(table.keys, std::less_equal{})) {
        return std::unexpected("keys are not strictly ascending");
    }
    if(auto ok = ids_ok(table.patterns, count, table.sym_hashes.size()); !ok) {
        return ok;
    }
    if(auto ok = ids_ok(table.resolutions, count, table.resolution_hashes.size() + 1); !ok) {
        return ok;
    }
    auto producers = table.producer_units.size();
    if(table.producer_runs.size() != producers) {
        return std::unexpected("producer columns do not line up");
    }
    if(llvm::any_of(table.producer_units, [](std::uint64_t unit) { return reserved_key(unit); })) {
        return std::unexpected("a reserved producer unit");
    }
    for(std::size_t i = 1; i < producers; i += 1) {
        if(std::pair(table.producer_units[i - 1], table.producer_runs[i - 1]) >=
           std::pair(table.producer_units[i], table.producer_runs[i])) {
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
CheckResult validate(BlobView root) {
    constexpr std::array checks = {symbols_ok, records_ok, resolutions_ok, uses_ok, keys_ok};
    for(auto check: checks) {
        if(auto ok = check(root); !ok) {
            return ok;
        }
    }
    return {};
}

}  // namespace

ResolutionHash resolution_identity(std::uint64_t content_hash,
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

UseHash use_identity(llvm::ArrayRef<ResolutionHash> resolutions) {
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
    auto root = BlobView::from_bytes(blob_bytes(buffer->getBuffer()));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting instantiation blob: structural verification failed");
        return {};
    }
    if(root[&InstantiationBlob::format_version] != index_format_version) {
        LOG_DEBUG("Rejecting instantiation blob: format version {}, this build reads {}",
                  root[&InstantiationBlob::format_version],
                  index_format_version);
        return {};
    }
    if(auto ok = validate(root); !ok) {
        LOG_DEBUG("Rejecting instantiation blob: {}", ok.error());
        return {};
    }
    InstantiationShard shard(std::move(buffer));
    auto columns = RecordTable::of(root).columns;
    for(std::uint32_t record = 0; record < columns.size(); record += 1) {
        shard.longest = std::max(shard.longest, columns.range_of(record).length());
    }
    return shard;
}

bool InstantiationShard::rebind(std::unique_ptr<llvm::MemoryBuffer> replacement) {
    if(!buffer || !replacement || replacement->getBufferSize() != buffer->getBufferSize()) {
        return false;
    }
    buffer = std::move(replacement);
    return true;
}

std::uint64_t InstantiationShard::content_hash() const {
    return buffer ? view_of(bytes())[&InstantiationBlob::content_hash] : 0;
}

llvm::ArrayRef<ResolutionHash> InstantiationShard::resolutions() const {
    if(!buffer) {
        return {};
    }
    return to_array_ref(view_of(bytes())[&InstantiationBlob::resolutions]);
}

llvm::ArrayRef<UseHash> InstantiationShard::uses() const {
    if(!buffer) {
        return {};
    }
    return to_array_ref(view_of(bytes())[&InstantiationBlob::uses]);
}

bool InstantiationShard::has_use(UseHash use) const {
    return std::ranges::binary_search(uses(), use);
}

std::vector<ResolutionHash> InstantiationShard::resolutions_of(UseHash use) const {
    std::vector<ResolutionHash> resolutions;
    if(!buffer) {
        return resolutions;
    }
    auto root = view_of(bytes());
    auto table = UseTable::of(root);
    auto hashes = to_array_ref(root[&InstantiationBlob::resolutions]);
    if(auto found = table.find(use)) {
        for(auto i = table.begin_of(*found); i < table.ends[*found]; i += 1) {
            resolutions.push_back(hashes[table.resolutions[i]]);
        }
    }
    return resolutions;
}

void InstantiationShard::set_live(llvm::ArrayRef<UseHash> live) {
    live_records.clear();
    dead_uses = false;
    if(!buffer) {
        return;
    }
    llvm::SmallVector<UseHash> sorted(live);
    llvm::sort(sorted);
    auto root = view_of(bytes());
    auto uses = UseTable::of(root);
    auto resolutions = ResolutionTable::of(root);
    std::vector<bool> live_resolutions(resolutions.hashes.size());
    for(std::uint32_t use = 0; use < uses.hashes.size(); use += 1) {
        if(!std::ranges::binary_search(sorted, uses.hashes[use])) {
            dead_uses = true;
            continue;
        }
        for(auto i = uses.begin_of(use); i < uses.ends[use]; i += 1) {
            live_resolutions[uses.resolutions[i]] = true;
        }
    }
    std::vector<std::uint64_t> bits((RecordTable::of(root).size() + 63) / 64);
    for(std::uint32_t resolution = 0; resolution < resolutions.hashes.size(); resolution += 1) {
        if(!live_resolutions[resolution]) {
            dead_uses = true;
            continue;
        }
        for(auto i = resolutions.begin_of(resolution); i < resolutions.ends[resolution]; i += 1) {
            auto record = resolutions.records[i];
            bits[record / 64] |= std::uint64_t(1) << (record % 64);
        }
    }
    if(dead_uses) {
        live_records = std::move(bits);
    }
}

bool InstantiationShard::has_dead_uses() const {
    return dead_uses;
}

llvm::ArrayRef<SymbolHash> InstantiationShard::keys() const {
    if(!buffer) {
        return {};
    }
    return to_array_ref(view_of(bytes())[&InstantiationBlob::keys]);
}

std::optional<InstantiationShard::Key> InstantiationShard::find_key(SymbolHash key) const {
    if(!buffer) {
        return std::nullopt;
    }
    auto table = KeyTable::of(view_of(bytes()));
    auto it = std::ranges::lower_bound(table.keys, key);
    if(it == table.keys.end() || *it != key) {
        return std::nullopt;
    }
    return table.at(static_cast<std::uint32_t>(it - table.keys.begin()));
}

void InstantiationShard::for_each_key(llvm::function_ref<bool(const Key&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = KeyTable::of(view_of(bytes()));
    for(std::uint32_t k = 0; k < table.keys.size(); k += 1) {
        if(!callback(table.at(k))) {
            return;
        }
    }
}

void InstantiationShard::lookup(
    std::uint32_t offset,
    llvm::function_ref<bool(const InstantiationRecord&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RecordTable::of(view_of(bytes()));
    // Records nest (a base specifier around the names in it), so the end
    // column is not monotonic: scan the records beginning close enough
    // before the offset for the longest of them to reach it.
    auto records = std::views::iota(std::uint32_t(0), static_cast<std::uint32_t>(table.size()));
    auto begin_of = [&](std::uint32_t record) {
        return table.columns.begin_of(record);
    };
    auto first =
        std::ranges::lower_bound(records, offset - std::min(offset, longest), {}, begin_of);
    auto last = std::ranges::upper_bound(records, offset, {}, begin_of);
    for(auto it = first; it != last; it += 1) {
        auto record = *it;
        if(live(record) && table.columns.range_of(record).touches(offset) &&
           !callback(table.at(record))) {
            return;
        }
    }
}

void InstantiationShard::lookup(
    SymbolHash symbol,
    llvm::function_ref<bool(const InstantiationRecord&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RecordTable::of(view_of(bytes()));
    auto it = std::ranges::lower_bound(table.sym_hashes, symbol);
    if(it == table.sym_hashes.end() || *it != symbol) {
        return;
    }
    auto id = static_cast<std::uint32_t>(it - table.sym_hashes.begin());
    for(std::uint32_t record = 0; record < table.size(); record += 1) {
        bool names = table.targets[record] == id ||
                     (has_from(table.kind_of(record)) && table.froms[record] == id);
        if(names && live(record) && !callback(table.at(record))) {
            return;
        }
    }
}

void InstantiationShard::for_each_record(
    llvm::function_ref<bool(const InstantiationRecord&)> callback) const {
    if(!buffer) {
        return;
    }
    auto table = RecordTable::of(view_of(bytes()));
    for(std::uint32_t record = 0; record < table.size(); record += 1) {
        if(live(record) && !callback(table.at(record))) {
            return;
        }
    }
}

namespace {

/// A key while a write accumulates it.
struct DraftKey {
    SymbolHash key = 0;
    SymbolHash pattern = 0;
    std::optional<ResolutionHash> resolution;
    std::optional<Producer> producer;
};

/// Everything a write accumulates before choosing column encodings.
struct Draft {
    std::uint64_t content_hash = 0;
    std::uint32_t content_size = 0;
    /// Ascending by identity, each resolution's records sorted and unique.
    std::vector<std::pair<ResolutionHash, std::vector<InstantiationRecord>>> resolutions;
    /// Ascending by key.
    std::vector<DraftKey> keys;
    /// Ascending by identity, each use's resolutions stored above.
    std::vector<std::pair<UseHash, std::vector<ResolutionHash>>> uses;
};

/// Sort and fold a resolution's records.
void canonicalize(std::vector<InstantiationRecord>& records) {
    std::ranges::sort(records, {}, record_key);
    records.erase(std::ranges::unique(records).begin(), records.end());
}

/// The single writer of InstantiationBlob: every encoding choice is a
/// function of the data.
void emit(const Draft& draft, llvm::raw_ostream& os) {
    // The records of every resolution, each stored once.
    std::vector<std::pair<InstantiationRecord, std::uint32_t>> held;
    for(std::uint32_t resolution = 0; resolution < draft.resolutions.size(); resolution += 1) {
        for(auto& record: draft.resolutions[resolution].second) {
            held.emplace_back(record, resolution);
        }
    }
    std::ranges::sort(held, {}, [](const auto& entry) {
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

    InstantiationBlob blob;
    blob.format_version = index_format_version;
    blob.content_hash = draft.content_hash;
    blob.content_size = draft.content_size;
    blob.sym_hashes.assign(referenced.begin(), referenced.end());
    std::ranges::sort(blob.sym_hashes);
    auto sym_id = [&](SymbolHash hash) {
        return static_cast<std::uint32_t>(std::ranges::lower_bound(blob.sym_hashes, hash) -
                                          blob.sym_hashes.begin());
    };

    RangeWriter ranges{.side = blob.records, .encoding = RangeEncoding::Delta};
    auto targets = id_writer(blob.record_targets, blob.sym_hashes.size());
    auto froms = id_writer(blob.record_froms, blob.sym_hashes.size());
    std::vector<std::vector<std::uint32_t>> members(draft.resolutions.size());
    for(std::size_t i = 0; i < held.size(); i += 1) {
        auto& [record, resolution] = held[i];
        if(i == 0 || held[i - 1].first != record) {
            ranges.add(record.range);
            blob.record_kinds.push_back(kind_byte(record));
            targets.add(sym_id(record.target));
            froms.add(has_from(record.kind) ? sym_id(record.from) : 0);
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
            auto it = std::ranges::lower_bound(blob.resolutions, resolution);
            assert(it != blob.resolutions.end() && *it == resolution &&
                   "a use's resolutions are stored");
            use_resolutions.push_back(static_cast<std::uint32_t>(it - blob.resolutions.begin()));
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
    std::ranges::sort(producers, {}, producer_order);
    producers.erase(std::ranges::unique(producers).begin(), producers.end());
    for(auto& producer: producers) {
        blob.producer_units.push_back(producer.unit);
        blob.producer_runs.push_back(producer.run);
    }

    auto patterns = id_writer(blob.key_patterns, blob.sym_hashes.size());
    auto resolutions = id_writer(blob.key_resolutions, blob.resolutions.size() + 1);
    auto key_producers = id_writer(blob.key_producers, producers.size());
    for(auto& key: draft.keys) {
        blob.keys.push_back(key.key);
        patterns.add(sym_id(key.pattern));
        std::uint32_t resolution = 0;
        if(key.resolution) {
            auto it = std::ranges::lower_bound(blob.resolutions, *key.resolution);
            assert(it != blob.resolutions.end() && *it == *key.resolution &&
                   "a key's resolution is stored");
            resolution = static_cast<std::uint32_t>(it - blob.resolutions.begin()) + 1;
        }
        resolutions.add(resolution);
        if(key.producer) {
            key_producers.add(
                static_cast<std::uint32_t>(std::ranges::lower_bound(producers,
                                                                    producer_order(*key.producer),
                                                                    {},
                                                                    producer_order) -
                                           producers.begin()));
        }
    }

    serialize_blob(blob, os);
}

using DecodedResolutions = std::vector<std::pair<ResolutionHash, std::vector<InstantiationRecord>>>;

/// The resolutions of a loaded blob `keep` accepts, with their records, in
/// stored order.
void decode_resolutions(const InstantiationShard& shard,
                        llvm::function_ref<bool(ResolutionHash)> keep,
                        DecodedResolutions& out) {
    auto root = view_of(shard.bytes());
    auto records = RecordTable::of(root);
    auto table = ResolutionTable::of(root);
    for(std::uint32_t resolution = 0; resolution < table.hashes.size(); resolution += 1) {
        if(!keep(table.hashes[resolution])) {
            continue;
        }
        out.push_back({table.hashes[resolution], {}});
        auto& list = out.back().second;
        for(auto i = table.begin_of(resolution); i < table.ends[resolution]; i += 1) {
            list.push_back(records.at(table.records[i]));
        }
    }
}

/// Order resolutions by identity and fold the ones stored twice.
void fold_resolutions(DecodedResolutions& resolutions) {
    auto identity = [](const auto& resolution) {
        return resolution.first;
    };
    std::ranges::stable_sort(resolutions, {}, identity);
    resolutions.erase(std::ranges::unique(resolutions, {}, identity).begin(), resolutions.end());
}

}  // namespace

void write_instantiations(const FileInstantiations& instantiations,
                          std::uint64_t content_hash,
                          std::uint32_t content_size,
                          llvm::raw_ostream& os) {
    Draft draft{.content_hash = content_hash, .content_size = content_size};
    std::vector<ResolutionHash> identities;
    for(auto records: instantiations.resolutions) {
        canonicalize(records);
        identities.push_back(resolution_identity(content_hash, records));
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
    std::ranges::sort(draft.keys, {}, &DraftKey::key);
    draft.keys.erase(std::ranges::unique(draft.keys, {}, &DraftKey::key).begin(), draft.keys.end());
    emit(draft, os);
}

void merge_instantiations(const InstantiationShard& old,
                          llvm::ArrayRef<UseHash> keep,
                          llvm::function_ref<bool(const Producer&)> keep_producer,
                          const InstantiationShard* fresh,
                          Producer producer,
                          std::optional<AddedUse> added,
                          llvm::raw_ostream& os) {
    assert((!fresh || !old.loaded() || old.content_hash() == fresh->content_hash()) &&
           "merge inputs must share one content generation");
    auto root = view_of((fresh ? *fresh : old).bytes());
    Draft draft{
        .content_hash = root[&InstantiationBlob::content_hash],
        .content_size = root[&InstantiationBlob::content_size],
    };

    // The uses kept, and the added one; their resolutions are the ones of
    // `old` that survive.
    llvm::SmallVector<UseHash> kept_uses(keep);
    llvm::sort(kept_uses);
    for(auto use: old.uses()) {
        if(std::ranges::binary_search(kept_uses, use)) {
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
    std::ranges::sort(draft.uses, {}, use_order);
    draft.uses.erase(std::ranges::unique(draft.uses, {}, use_order).begin(), draft.uses.end());
    llvm::DenseSet<ResolutionHash> used;
    for(auto& resolutions: llvm::make_second_range(draft.uses)) {
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
               std::ranges::binary_search(draft.resolutions,
                                          *resolution,
                                          {},
                                          [](const auto& entry) { return entry.first; });
    };

    std::vector<DraftKey> walked;
    if(fresh) {
        fresh->for_each_key([&](const InstantiationShard::Key& key) {
            walked.push_back({.key = key.key,
                              .pattern = key.pattern,
                              .resolution = key.resolution,
                              .producer = producer});
            return true;
        });
    }
    std::vector<DraftKey> kept;
    old.for_each_key([&](const InstantiationShard::Key& key) {
        if(keep_producer(*key.producer) && stored(key.resolution) &&
           !std::ranges::binary_search(walked, key.key, {}, &DraftKey::key)) {
            kept.push_back({.key = key.key,
                            .pattern = key.pattern,
                            .resolution = key.resolution,
                            .producer = key.producer});
        }
        return true;
    });
    std::ranges::merge(kept,
                       walked,
                       std::back_inserter(draft.keys),
                       {},
                       &DraftKey::key,
                       &DraftKey::key);
    assert((!draft.resolutions.empty() || !draft.keys.empty()) &&
           "a file left with nothing is retired, not merged");
    emit(draft, os);
}

}  // namespace clice::index
