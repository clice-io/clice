module;

#include "modules/prelude.h"

module clice;

import :index.instantiation_shard;
import :index.serialization;
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

InstantiationShard make(llvm::StringRef bytes) {
    return InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
}

InstantiationShard write(const FileInstantiations& instantiations) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_instantiations(instantiations, generation, size, os);
    return make(bytes);
}

InstantiationShard merge(
    const InstantiationShard& old,
    llvm::ArrayRef<ResolutionHash> keep,
    const InstantiationShard* fresh,
    Producer producer,
    llvm::function_ref<bool(const Producer&)> keep_producer = [](const Producer&) {
        return true;
    }) {
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::merge_instantiations(old, keep, keep_producer, fresh, producer, os);
    return make(bytes);
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
    shard.lookup(offset, [&](const InstantiationRecord& record) {
        records.push_back(record);
        return true;
    });
    return records;
}

std::vector<InstantiationRecord> naming(const InstantiationShard& shard, index::SymbolHash symbol) {
    std::vector<InstantiationRecord> records;
    shard.lookup(symbol, [&](const InstantiationRecord& record) {
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
        index::resolution_identity(generation,
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

    auto stored = merge({}, {}, &a, one);
    stored = merge(stored, stored.resolutions(), &b, two);
    ZASSERT(stored.loaded());
    ZASSERT(stored.resolutions().size() == 2u);
    ZEXPECT(records_of(stored).size() == 3u);

    // The later run walked key 1 again: it is the producer now.
    ZEXPECT(stored.find_key(1)->producer == std::optional(two));
    ZEXPECT(stored.find_key(2)->producer == std::optional(two));

    // Only the resolution holding (30, 33) dies: the shared record stays.
    auto first_resolution = a.resolutions().front();
    stored.set_live({first_resolution});
    ZEXPECT(stored.has_dead_resolutions());
    ZEXPECT(at(stored, 11).size() == 1u);
    ZEXPECT(at(stored, 31).empty());

    // Compaction drops the dead resolution, its records and its key.
    auto compacted = merge(stored, {first_resolution}, nullptr, {});
    ZASSERT(compacted.loaded());
    ZEXPECT(compacted.resolutions().size() == 1u);
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

ZEST_CASE(CorruptBlobRejected) {
    ZEXPECT(!InstantiationShard::from_bytes("not a flatbuffer").loaded());

    index::InstantiationBlob blob;
    blob.format_version = index::index_format_version;
    blob.content_hash = generation;
    blob.content_size = size;
    blob.sym_hashes = {11};
    blob.records.packed = {index::pack_range(10, 3)};
    blob.record_kinds = {0};
    blob.record_targets.ids8 = {0};
    blob.record_froms.ids8 = {0};
    blob.resolutions = {5};
    blob.resolution_ends = {1};
    blob.resolution_records.ids8 = {0};
    auto bytes_of = [&] {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_blob(blob, os);
        return bytes;
    };
    ZASSERT(make(bytes_of()).loaded());

    // A record no resolution holds.
    blob.resolution_ends = {0};
    blob.resolution_records.ids8 = {};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.resolution_ends = {1};
    blob.resolution_records.ids8 = {0};

    // A record past the content.
    blob.records.packed = {index::pack_range(size, 3)};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.records.packed = {index::pack_range(10, 3)};

    // A key whose resolution is not stored.
    blob.keys = {1};
    blob.key_patterns.ids8 = {0};
    blob.key_resolutions.ids8 = {2};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.key_resolutions.ids8 = {1};
    ZEXPECT(make(bytes_of()).loaded());

    // Hashes that key DenseMaps once decoded.
    blob.sym_hashes = {~0ull};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.sym_hashes = {11};
    blob.resolutions = {~0ull};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.resolutions = {5};

    // Producers name every key or none.
    blob.producer_units = {100};
    blob.producer_runs = {1};
    ZEXPECT(!make(bytes_of()).loaded());
    blob.key_producers.ids8 = {0};
    ZEXPECT(make(bytes_of()).loaded());
    blob.producer_units = {~0ull - 1};
    ZEXPECT(!make(bytes_of()).loaded());
}

};  // ZEST_SUITE(InstantiationShard)

}  // namespace
}  // namespace clice::testing
