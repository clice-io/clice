module;

#include "modules/prelude.h"

module clice;

import :index.format;
import :index.indexer;
import :index.shard;
import :index.tu_index;
import :unittests.utils.tester;
import :unittests.utils.tu_index_blob;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(TUIndex, Tester) {

/// Compile the files added so far and load the TU index built from them.
index::TUIndex build() {
    ZASSERT(compile());
    return index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit)));
}

ZEST_CASE(TUIndexSections) {
    kind = CompilationKind::Whole;
    add_file("header.h", R"(
            #pragma once
            inline int §(hdr)helper() { return 1; }
        )");
    add_focus("main.cpp", R"(
            #include "header.h"
            int main() { return §(use)helper(); }
        )");
    auto view = build();
    ZASSERT(view.section_count() > 1);
    ZASSERT(view.built_at() > 0);

    // Sections ascend by path id and the focus file's shard is the
    // last path id's section.
    for(std::uint32_t i = 1; i < view.section_count(); i += 1) {
        ZASSERT(view.section_path(i - 1) < view.section_path(i));
    }
    auto focus_section = view.section_of(view.path_count() - 1);
    ZASSERT(focus_section);

    // Every section's hash is the byte identity of its blob, the blob
    // loads as a self-contained single-variant shard under that identity,
    // and the whole TU index passes the persisted-load gate.
    ZASSERT(view.verify_shards());
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        auto blob = view.section_blob(i);
        ZASSERT(view.section_hash(i) == llvm::xxh3_64bits(blob));
        auto shard = index::Shard::from_bytes(blob);
        ZASSERT(shard.loaded());
        ZASSERT(shard.variants().size() == std::size_t(1));
        ZASSERT(shard.variants().front() == view.section_hash(i));
    }

    // The graph travels with the TU index: every path resolves and the
    // consumed-content hash column covers the whole table.
    for(std::uint32_t id = 0; id < view.path_count(); id += 1) {
        ZASSERT(!view.path(id).empty());
        ZASSERT(view.content_hash(id) != 0);
    }

    // Symbols read back identically through iteration and point lookup.
    std::uint32_t symbols = 0;
    view.for_each_symbol([&](index::SymbolHash hash,
                             const index::SymbolView& symbol,
                             llvm::ArrayRef<std::uint32_t>) {
        auto found = view.find_symbol(hash);
        ZEXPECT(found);
        if(found) {
            ZEXPECT(found->name == symbol.name);
            ZEXPECT(found->kind.value() == symbol.kind.value());
            ZEXPECT(static_cast<int>(found->scope) == static_cast<int>(symbol.scope));
        }
        symbols += 1;
        return true;
    });
    ZASSERT(symbols > 0);
}

ZEST_CASE(CorruptSectionBytesRejected) {
    // The persisted-load gate must reject any flipped section byte, in
    // particular flips that still form a structurally valid shard (an
    // opaque hash field, say) — only the byte hash catches those, and
    // serving them would navigate by corrupted entries across restarts.
    add_focus("main.cpp", R"(
            int value = 1;
            int main() { return value; }
        )");
    auto view = build();
    ZASSERT(view.section_count() > 0);
    auto blob = view.section_blob(0);
    auto offset = static_cast<std::size_t>(blob.data() - view.bytes().data());

    bool structurally_valid_flip = false;
    std::string bytes = view.bytes().str();
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        std::string mutated = bytes;
        mutated[offset + i] ^= 0x01;
        auto reloaded = index::TUIndex::from_bytes(mutated);
        if(!reloaded.loaded()) {
            continue;
        }
        if(index::Shard::from_bytes(reloaded.section_blob(0)).loaded()) {
            structurally_valid_flip = true;
        }
        ZASSERT(!reloaded.verify_shards());
    }
    ZASSERT(structurally_valid_flip);
}

ZEST_CASE(FromRejectsHostileInput) {
    ZASSERT(!index::TUIndex::from_bytes("not a flatbuffer at all").loaded());

    add_focus("main.cpp", R"(
            int foo() { return 42; }
        )");
    auto view = build();
    auto bytes = view.bytes();

    // Sanity: the intact TU index loads, so the rejections below are earned.
    ZASSERT(index::TUIndex::from_bytes(bytes).loaded());

    ZASSERT(!index::TUIndex::from_bytes(bytes.substr(0, bytes.size() / 2)).loaded());

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    ZASSERT(bytes.size() > 8);
    std::string clobbered(bytes.data(), bytes.size());
    for(std::size_t i = 4; i < 8; i += 1) {
        clobbered[i] = 'X';
    }
    ZASSERT(!index::TUIndex::from_bytes(clobbered).loaded());
}

ZEST_CASE(FromRejectsStaleFormatVersion) {
    // Only the version slot and the path table are written: every other
    // field reads back absent, which is structurally valid — the result
    // must hinge on the version value.
    auto stale = tu_index_blob();
    stale.format_version = index::index_format_version + 1;
    stale.paths = {"/proj/main.cpp"};
    ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(stale)).loaded());

    // Positive control: the same shape carrying the current version loads,
    // so the rejection above comes from the value, not the blob's shape.
    auto current = stale;
    current.format_version = index::index_format_version;
    ZASSERT(index::TUIndex::from_bytes(tu_index_bytes(current)).loaded());
}

ZEST_CASE(FromRejectsReservedParents) {
    // Symbol parents become DenseSet keys in a query's container walk; the
    // sentinel values must fail the TU index as a whole.
    auto honest = tu_index_blob();
    honest.paths = {"/proj/main.cpp"};
    add_symbol(honest, 42, {.name = "sym", .parent = 7});
    ZASSERT(index::TUIndex::from_bytes(tu_index_bytes(honest)).loaded());

    auto hostile = honest;
    hostile.symbol_parents[0] = ~std::uint64_t(0);
    ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    hostile.symbol_parents[0] = ~std::uint64_t(0) - 1;
    ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
}

ZEST_CASE(FromRejectsSymbolColumns) {
    // Merged reference files persist behind versions that match the disk,
    // so a malformed table must fail the TU index as a whole, never merge
    // in part.
    auto honest = tu_index_blob();
    honest.paths = {"/proj/a.h", "/proj/main.cpp"};
    add_symbol(honest, 42, {.name = "first", .file = 0}, {0, 1});
    add_symbol(honest, 43, {.name = "second"}, {1});
    auto bytes = tu_index_bytes(honest);
    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());
    auto second = view.find_symbol(43);
    ZASSERT(second);
    ZASSERT(second->name == "second");

    {
        // Lookups binary-search the hashes.
        auto hostile = honest;
        std::swap(hostile.symbol_hashes[0], hostile.symbol_hashes[1]);
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = honest;
        hostile.symbol_hashes[1] = hostile.symbol_hashes[0];
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = honest;
        hostile.symbol_hashes[1] = ~std::uint64_t(0);
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = honest;
        hostile.symbol_references.back() = 2;
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = honest;
        hostile.symbol_files[1] = 2;
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        // Token kinds index the legend's tables.
        auto hostile = honest;
        hostile.symbol_token_kinds[1] = 200;
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = honest;
        hostile.symbol_name_ends[0] = 100;
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = honest;
        hostile.symbol_kinds.pop_back();
        ZEXPECT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
}

ZEST_CASE(FromRejectsOutOfRangeParents) {
    // A node's parent indexes the node table in every consumer; only
    // another node or the root sentinel is acceptable.
    auto honest = tu_index_blob();
    honest.paths = {"/proj/main.cpp", "/proj/a.h"};
    honest.nodes.push_back({.file = 1, .parent = ~0u, .line = 1});
    honest.nodes.push_back({.file = 1, .parent = 0, .line = 2});
    ZASSERT(index::TUIndex::from_bytes(tu_index_bytes(honest)).loaded());

    auto hostile = honest;
    hostile.nodes.back().parent = 2;
    ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
}

ZEST_CASE(FromRejectsOutOfRangePathIds) {
    // Structural verification does not constrain field values, and the
    // merge pipeline dereferences every decoded path id against the path
    // table without further checks — a TU index pointing outside its own
    // table must be rejected as a whole (the symbol table's ids: see
    // FromRejectsSymbolColumns).

    // Positive control first: the same shapes with in-range ids load, so
    // the rejections below come from the hostile values.
    auto honest = tu_index_blob();
    honest.paths = {"/proj/main.cpp"};
    honest.nodes.push_back({.file = 0, .parent = 0, .line = 1});
    honest.sections.push_back({.path_id = 0});
    ZASSERT(index::TUIndex::from_bytes(tu_index_bytes(honest)).loaded());

    {
        auto hostile = tu_index_blob();
        hostile.paths = {"/proj/main.cpp"};
        hostile.nodes.push_back({.file = 7, .parent = 0, .line = 1});
        ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = tu_index_blob();
        hostile.paths = {"/proj/main.cpp"};
        hostile.sections.push_back({.path_id = 7});  // Only path id 0 exists.
        ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
}

ZEST_CASE(FromRejectsEmptyPathTable) {
    // The builder ends every path table with the focus file, and
    // consumers address path_count() - 1 unchecked — a TU index with no
    // paths at all is corrupt.
    auto hostile = tu_index_blob();
    ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
}

ZEST_CASE(FromRejectsUnsortedSections) {
    // section_of binary-searches the section table by path id; a repeated
    // or out-of-order id would attribute one file's shard to another.

    // Positive control: the ascending shape loads.
    auto honest = tu_index_blob();
    honest.paths = {"/proj/a.h", "/proj/main.cpp"};
    honest.sections.push_back({.path_id = 0});
    honest.sections.push_back({.path_id = 1});
    ZASSERT(index::TUIndex::from_bytes(tu_index_bytes(honest)).loaded());

    {
        auto hostile = tu_index_blob();
        hostile.paths = {"/proj/a.h", "/proj/main.cpp"};
        hostile.sections.push_back({.path_id = 1});
        hostile.sections.push_back({.path_id = 0});
        ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
    {
        auto hostile = tu_index_blob();
        hostile.paths = {"/proj/a.h", "/proj/main.cpp"};
        hostile.sections.push_back({.path_id = 1});
        hostile.sections.push_back({.path_id = 1});
        ZASSERT(!index::TUIndex::from_bytes(tu_index_bytes(hostile)).loaded());
    }
}

ZEST_CASE(AbsentContentHashesReadZero) {
    // The hash column may be shorter than the path table on a foreign
    // TU index (structurally valid: the field reads back empty); absent
    // entries read as 0, "unavailable".
    auto blob = tu_index_blob();
    blob.paths = {"/proj/a.h", "/proj/main.cpp"};
    blob.content_hashes = {7};
    auto bytes = tu_index_bytes(blob);
    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());
    ZASSERT(view.content_hash(0) == 7u);
    ZASSERT(view.content_hash(1) == 0u);
}

};  // ZEST_SUITE(TUIndex)

}  // namespace

}  // namespace clice::testing
