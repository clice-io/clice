module;

#include "support/prelude.h"

module clice;

import :feature.index_entries;
import :index.shard;
import :index.symbol;
import :semantic.symbol;

namespace clice::feature {

IndexEntries extract_index_entries(const index::Shard& shard) {
    IndexEntries entries;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        entries.occurrences.push_back(occurrence);
        return true;
    });
    auto written = entries.occurrences;
    ranges::sort(written, {}, [](const index::Occurrence& o) { return o.range.begin; });
    auto nested = [&](LocalSourceRange range) {
        auto it = ranges::upper_bound(written, range.begin, {}, [](const index::Occurrence& o) {
            return o.range.begin;
        });
        return it != written.begin() && std::prev(it)->range.contains(range) &&
               std::prev(it)->range != range;
    };
    std::vector<index::Occurrence> names;
    shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        RelationKind kind(relation.kind);
        if(kind.carries_extent()) {
            auto copy = relation;
            auto& into = kind.isDeclOrDef() ? entries.decls : entries.instantiations;
            into.push_back({.range = relation.range,
                            .extent = copy.extent(),
                            .symbol = hash,
                            .definition = kind.is_one_of(RelationKind::Definition)});
        } else if(kind.isReference() && nested(relation.range)) {
            names.push_back({.range = relation.range, .target = hash});
        }
        if(kind.is_one_of(RelationKind::Write)) {
            entries.writes.push_back({.range = relation.range, .target = hash});
        }
        return true;
    });
    entries.occurrences.append_range(names);
    return entries;
}

}  // namespace clice::feature
