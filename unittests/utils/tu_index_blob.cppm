module;

#include "modules/prelude.h"

module clice:unittests.utils.tu_index_blob;

import :index.format;
import :index.symbol;
import :index.tu_index;
import :unittests.utils;

namespace clice::testing {

/// An empty TU index blob of the current format, for tests that plant
/// shapes the builder never emits.
inline index::TUIndexBlob tu_index_blob() {
    return {.format_version = index::index_format_version};
}

/// Append a symbol entry; the reader requires ascending hashes.
inline void add_symbol(index::TUIndexBlob& blob,
                       index::SymbolHash hash,
                       const index::SymbolView& symbol,
                       llvm::ArrayRef<std::uint32_t> references = {},
                       llvm::ArrayRef<std::uint32_t> instantiation_files = {}) {
    blob.symbol_hashes.push_back(hash);
    blob.symbol_names += symbol.name;
    blob.symbol_name_ends.push_back(static_cast<std::uint32_t>(blob.symbol_names.size()));
    blob.symbol_args += symbol.args;
    blob.symbol_args_ends.push_back(static_cast<std::uint32_t>(blob.symbol_args.size()));
    blob.symbol_signatures += symbol.signature;
    blob.symbol_signature_ends.push_back(static_cast<std::uint32_t>(blob.symbol_signatures.size()));
    blob.symbol_parents.push_back(symbol.parent);
    blob.symbol_kinds.push_back(symbol.kind.value());
    blob.symbol_scopes.push_back(static_cast<std::uint8_t>(symbol.scope));
    blob.symbol_flags.push_back(static_cast<std::uint16_t>(symbol.flags));
    blob.symbol_token_kinds.push_back(static_cast<std::uint8_t>(symbol.token_class.kind));
    blob.symbol_token_modifiers.push_back(symbol.token_class.modifiers);
    blob.symbol_files.push_back(symbol.file);
    llvm::append_range(blob.symbol_references, references);
    blob.symbol_reference_ends.push_back(static_cast<std::uint32_t>(blob.symbol_references.size()));
    llvm::append_range(blob.symbol_instantiations, instantiation_files);
    blob.symbol_instantiation_ends.push_back(
        static_cast<std::uint32_t>(blob.symbol_instantiations.size()));
}

/// A loaded TU index's fields, to re-encode with one of them changed. The
/// preamble ride-alongs are left out.
inline index::TUIndexBlob tu_index_blob(const index::TUIndex& view) {
    auto blob = tu_index_blob();
    blob.built_at = view.built_at();
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        blob.paths.emplace_back(view.path(i));
        blob.content_hashes.push_back(view.content_hash(i));
    }
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        blob.nodes.push_back(view.node(i));
    }
    llvm::DenseMap<index::SymbolHash, llvm::ArrayRef<std::uint32_t>> instantiation_files;
    view.for_each_instantiation_file(
        [&](index::SymbolHash hash, llvm::ArrayRef<std::uint32_t> files) {
            instantiation_files[hash] = files;
            return true;
        });
    view.for_each_symbol([&](index::SymbolHash hash,
                             const index::SymbolView& symbol,
                             llvm::ArrayRef<std::uint32_t> references) {
        add_symbol(blob, hash, symbol, references, instantiation_files.lookup(hash));
        return true;
    });
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        auto bytes = view.section_blob(i);
        blob.sections.push_back({view.section_path(i),
                                 view.section_hash(i),
                                 std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
    }
    for(std::uint32_t i = 0; i < view.instantiation_count(); i += 1) {
        auto bytes = view.instantiation_blob(i);
        auto skipped = view.skipped_instantiations(i);
        blob.instantiations.push_back({
            view.instantiation_path(i),
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            {skipped.begin(), skipped.end()}
        });
    }
    return blob;
}

/// The blob's bytes, as a worker sends them.
inline std::string tu_index_bytes(const index::TUIndexBlob& blob) {
    auto encoded = kota::codec::fbs::to_bytes(blob);
    ZASSERT(encoded);
    return std::string(encoded->begin(), encoded->end());
}

}  // namespace clice::testing
