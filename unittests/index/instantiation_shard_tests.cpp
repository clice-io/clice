module;

#include "modules/prelude.h"

module clice;

import :index.format;
import :index.instantiation_shard;
import :support.bytes;
import :unittests.utils;

namespace clice::testing {
namespace {

using index::ResolutionHash;
using index::FileInstantiations;
using index::InstantiationKind;
using index::InstantiationRecord;
using index::InstantiationShard;
using index::Producer;

constexpr std::uint64_t generation = 0x1234;
constexpr std::uint32_t size = 1000;

InstantiationRecord reference(std::uint32_t begin, index::SymbolHash target) {
    return {
        .range = {begin, begin + 3},
        .kind = InstantiationKind::Reference,
        .target = target
    };
}

InstantiationRecord call(std::uint32_t begin, index::SymbolHash target, index::SymbolHash from) {
    return {
        .range = {begin, begin + 3},
        .kind = InstantiationKind::Call,
        .target = target,
        .from = from
    };
}

InstantiationShard make(std::span<const std::byte> bytes) {
    return InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
}

InstantiationShard write(const FileInstantiations& instantiations) {
    auto bytes = index::write_instantiations(instantiations, generation, size);
    return make(bytes);
}

constexpr auto any_producer = [](const Producer&) {
    return true;
};

InstantiationShard merge(const InstantiationShard& old,
                         llvm::ArrayRef<index::UseHash> keep,
                         const InstantiationShard* fresh,
                         Producer producer,
                         llvm::function_ref<bool(const Producer&)> keep_producer = any_producer,
                         std::optional<index::AddedUse> added = std::nullopt) {
    auto bytes = index::merge_instantiations(old, keep, keep_producer, fresh, producer, added)
                     .value_or(std::vector<std::byte>());
    return make(bytes);
}

/// The use of a unit using every resolution of `shard`.
index::AddedUse use_of(const InstantiationShard& shard) {
    return {.use = index::use_hash(shard.resolutions()), .resolutions = shard.resolutions()};
}

std::vector<InstantiationRecord> records_of(const InstantiationShard& shard) {
    std::vector<InstantiationRecord> records;
    shard.for_each_record([&](const InstantiationRecord& record) {
        records.push_back(record);
        return true;
    });
    return records;
}

std::vector<InstantiationRecord> at(const InstantiationShard& shard, std::uint32_t offset) {
    std::vector<InstantiationRecord> records;
    shard.records_at(offset, [&](const InstantiationRecord& record) {
        records.push_back(record);
        return true;
    });
    return records;
}

std::vector<InstantiationRecord> naming(const InstantiationShard& shard, index::SymbolHash symbol) {
    std::vector<InstantiationRecord> records;
    shard.records_of(symbol, [&](const InstantiationRecord& record) {
        records.push_back(record);
        return true;
    });
    return records;
}

ZEST_SUITE(InstantiationShard) {

ZEST_CASE(Roundtrip) {
    // `t.go()` resolved to 11 calling it from 50, and `t.size` to 22.
    FileInstantiations instantiations;
    instantiations.resolutions = {
        {call(10, 11, 50), reference(10, 11), reference(20, 22)},
    };
    instantiations.keys = {
        {.key = 7, .pattern = 50, .resolution = 0},
        {.key = 8, .pattern = 50},
    };
    auto shard = write(instantiations);
    ZASSERT(shard.loaded());
    ZEXPECT(shard.content_hash() == generation);

    auto identity =
        index::resolution_hash(generation,
                               {reference(10, 11), call(10, 11, 50), reference(20, 22)});
    ZASSERT(shard.resolutions().size() == 1u);
    ZEXPECT(shard.resolutions().front() == identity);

    auto found = at(shard, 11);
    ZASSERT(found.size() == 2u);
    ZEXPECT(found[0] == reference(10, 11));
    ZEXPECT(found[1] == call(10, 11, 50));
    ZEXPECT(at(shard, 5).empty());

    ZEXPECT(naming(shard, 50) == std::vector{call(10, 11, 50)});
    ZEXPECT(naming(shard, 22) == std::vector{reference(20, 22)});

    auto with_resolution = shard.find_key(7);
    ZASSERT(with_resolution);
    ZEXPECT(with_resolution->pattern == 50u);
    ZEXPECT(with_resolution->resolution == std::optional(identity));
    ZEXPECT(!with_resolution->producer);
    auto empty = shard.find_key(8);
    ZASSERT(empty);
    ZEXPECT(!empty->resolution);
    ZEXPECT(!shard.find_key(9));
}

ZEST_CASE(LookupAcrossBlocks) {
    // Records span several blocks of their delta-encoded begins, one of
    // them long enough to hold offsets far past its begin.
    std::vector<InstantiationRecord> held;
    for(std::uint32_t i = 0; i < 60; i += 1) {
        held.push_back(reference(i * 15, 100 + i));
    }
    InstantiationRecord outer{
        .range = {16, 400},
        .kind = InstantiationKind::Reference,
        .target = 99
    };
    held.push_back(outer);
    FileInstantiations instantiations;
    instantiations.resolutions = {held};
    auto shard = write(instantiations);
    ZASSERT(shard.loaded());
    for(std::uint32_t i = 0; i < 60; i += 1) {
        auto offset = i * 15 + 1;
        auto found = at(shard, offset);
        ZEXPECT(found.size() == (offset >= 16 && offset <= 400 ? 2u : 1u));
        ZEXPECT(std::ranges::contains(found, reference(i * 15, 100 + i)));
        ZEXPECT(naming(shard, 100 + i) == std::vector{reference(i * 15, 100 + i)});
    }
    ZEXPECT(at(shard, 399) == std::vector{outer});
}

ZEST_CASE(ResolutionsFoldAcrossRuns) {
    // Two runs with the same resolution store it once; a record two
    // resolutions share is stored once and serves while either lives.
    FileInstantiations first;
    first.resolutions = {
        {reference(10, 11), reference(20, 22)}
    };
    first.keys = {
        {.key = 1, .pattern = 50, .resolution = 0}
    };
    FileInstantiations second;
    second.resolutions = {
        {reference(10, 11), reference(20, 22)},
        {reference(10, 11), reference(30, 33)},
    };
    second.keys = {
        {.key = 1, .pattern = 50, .resolution = 0},
        {.key = 2, .pattern = 50, .resolution = 1},
    };
    auto a = write(first);
    auto b = write(second);
    Producer one{.unit = 100, .run = 1};
    Producer two{.unit = 200, .run = 1};

    auto first_use = use_of(a);
    auto stored = merge({}, {}, &a, one, any_producer, first_use);
    auto stored_uses = std::vector(stored.uses().begin(), stored.uses().end());
    stored = merge(stored, stored_uses, &b, two, any_producer, use_of(b));
    ZASSERT(stored.loaded());
    ZASSERT(stored.resolutions().size() == 2u);
    ZEXPECT(records_of(stored).size() == 3u);
    ZASSERT(stored.uses().size() == 2u);

    // The later run walked key 1 again: it is the producer now.
    ZEXPECT(stored.find_key(1)->producer == std::optional(two));
    ZEXPECT(stored.find_key(2)->producer == std::optional(two));

    // Only the second unit's use dies, and with it the resolution holding
    // (30, 33): the shared record stays.
    stored.set_live({first_use.use});
    ZEXPECT(stored.has_dead_uses());
    ZEXPECT(at(stored, 11).size() == 1u);
    ZEXPECT(at(stored, 31).empty());

    // Compaction drops the dead use, the resolution only it held, its
    // records and its key.
    auto compacted = merge(stored, {first_use.use}, nullptr, {});
    ZASSERT(compacted.loaded());
    ZEXPECT(compacted.resolutions().size() == 1u);
    ZEXPECT(compacted.uses().size() == 1u);
    ZEXPECT(records_of(compacted).size() == 2u);
    ZEXPECT(compacted.find_key(1).has_value());
    ZEXPECT(!compacted.find_key(2).has_value());
}

ZEST_CASE(KeysOfGoneRunsDrop) {
    FileInstantiations walked;
    walked.keys = {
        {.key = 1, .pattern = 50},
        {.key = 2, .pattern = 51},
    };
    auto fresh = write(walked);
    Producer old_run{.unit = 100, .run = 1};
    auto stored = merge({}, {}, &fresh, old_run);
    ZASSERT(stored.keys().size() == 2u);

    // The run is no longer its unit's last: its keys leave at the next
    // merge.
    FileInstantiations other;
    other.keys = {
        {.key = 3, .pattern = 52}
    };
    auto next = write(other);
    auto merged = merge(stored, {}, &next, {.unit = 200, .run = 5}, [&](const Producer& producer) {
        return producer != old_run;
    });
    ZASSERT(merged.loaded());
    ZEXPECT(merged.keys().size() == 1u);
    ZEXPECT(merged.find_key(3)->producer->unit == 200u);
}

ZEST_CASE(NestedRecordsFound) {
    // A base specifier spans the names inside it.
    FileInstantiations instantiations;
    InstantiationRecord base{
        .range = {10, 40},
        .kind = InstantiationKind::Base,
        .target = 11,
        .from = 50
    };
    instantiations.resolutions = {
        {base, reference(30, 22)}
    };
    auto shard = write(instantiations);
    ZASSERT(shard.loaded());
    auto found = at(shard, 31);
    ZASSERT(found.size() == 2u);
    ZEXPECT(found[0] == base);
    ZEXPECT(at(shard, 12) == std::vector{base});
}

ZEST_CASE(ExpansionRecordsKeptApart) {
    // A name a macro body spells and the same name an instantiation
    // resolves are two records.
    auto expanded = reference(10, 11);
    expanded.expansion = true;
    FileInstantiations instantiations;
    instantiations.resolutions = {{reference(10, 11)}, {expanded}};
    auto shard = write(instantiations);
    ZASSERT(shard.loaded());
    ZEXPECT(shard.resolutions().size() == 2u);
    ZEXPECT(at(shard, 11) == std::vector{reference(10, 11), expanded});
}

ZEST_CASE(MisalignedBytesLoad) {
    // A TU index's instantiation sections sit at whatever offset its layout
    // gave them, while their columns are read in place.
    FileInstantiations instantiations;
    instantiations.resolutions = {
        {reference(10, 11)},
    };
    auto bytes = index::write_instantiations(instantiations, generation, size);
    std::vector<std::uint64_t> storage(bytes.size() / sizeof(std::uint64_t) + 2);
    auto* misaligned = reinterpret_cast<char*>(storage.data()) + sizeof(std::uint32_t);
    std::memcpy(misaligned, bytes.data(), bytes.size());

    auto shard = InstantiationShard::from_bytes(llvm::StringRef(misaligned, bytes.size()));
    ZASSERT(shard.loaded());
    ZEXPECT(at(shard, 11) == std::vector{reference(10, 11)});
}

ZEST_CASE(CorruptBlobRejected) {
    ZEXPECT(!InstantiationShard::from_bytes("not a blob").loaded());

    index::InstantiationShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content_hash = generation;
    blob.content_size = size;
    blob.symbol_hashes = {11};
    blob.records.block_begins = {10};
    blob.records.deltas = {0};
    blob.records.lengths = {3};
    blob.record_kinds = {0};
    blob.record_targets = {0};
    blob.record_froms = {0};
    blob.resolutions = {5};
    blob.resolution_ends = {1};
    blob.resolution_records = {0};
    auto bytes_of = [&] {
        return index::encode(blob);
    };
    ZASSERT(make(bytes_of()).loaded());

    // A record no resolution holds.
    blob.resolution_ends = {0};
    blob.resolution_records = {};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.resolution_ends = {1};
    blob.resolution_records = {0};

    // A record past the content.
    blob.records.block_begins = {size};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.records.block_begins = {10};

    // A key whose resolution is not stored.
    blob.keys = {1};
    blob.key_patterns = {0};
    blob.key_resolutions = {2};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.key_resolutions = {1};
    ZEXPECT(make(bytes_of()).loaded());

    // Hashes that key DenseMaps once decoded.
    blob.symbol_hashes = {~0ull};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.symbol_hashes = {11};
    blob.resolutions = {~0ull};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.resolutions = {5};

    // Producers name every key or none.
    blob.producer_units = {100};
    blob.producer_runs = {1};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.key_producers = {0};
    ZEXPECT(make(bytes_of()).loaded());
    blob.producer_units = {~0ull - 1};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.producer_units = {100};

    // A use names stored resolutions under the identity they hash to.
    std::vector<ResolutionHash> used = {5};
    blob.uses = {index::use_hash(used)};
    blob.use_ends = {1};
    blob.use_resolutions = {0};
    ZEXPECT(make(bytes_of()).loaded());
    blob.uses = {index::use_hash(used) + 1};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.uses = {index::use_hash(used)};
    blob.use_resolutions = {1};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.use_resolutions = {0};

    // Use lists partition the resolution column.
    blob.use_ends = {2};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.use_ends = {0};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.use_ends = {1};
    ZEXPECT(make(bytes_of()).loaded());
}

};  // ZEST_SUITE(InstantiationShard)

}  // namespace
}  // namespace clice::testing
