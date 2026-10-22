module;

#include "modules/prelude.h"

module clice:unittests.utils.index_entries_tester;

import :feature.index_entries;
import :index.indexer;
import :index.symbol;
import :index.tu_index;
import :support.bytes;
import :unittests.utils.tester;

namespace clice::testing {

/// The compiled unit's TU index and the focus file's entries, extracted the
/// way the router's index slice does it in production.
struct IndexEntriesTester : Tester {
    index::TUIndex tu;

    std::vector<std::byte> bytes;

    feature::IndexEntries entries;

    void extract_entries() {
        bytes = index::build_tu_index(*unit);
        tu = index::TUIndex::from_bytes(as_chars(bytes));

        auto focus_id = tu.path_count() - 1;
        entries = feature::extract_index_entries(tu.shard_of(focus_id));
    }

    std::optional<index::SymbolInfo> resolve(index::SymbolHash hash) {
        if(auto identity = tu.find_symbol(hash)) {
            return index::SymbolInfo::from(hash, *identity);
        }
        auto focus_id = tu.path_count() - 1;
        if(auto identity = tu.shard_of(focus_id).find_symbol(hash)) {
            return index::SymbolInfo::from(hash, *identity);
        }
        return std::nullopt;
    }

    auto resolver() {
        return [this](index::SymbolHash hash) {
            return resolve(hash);
        };
    }
};

}  // namespace clice::testing
