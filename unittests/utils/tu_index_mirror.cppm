module;

#include "modules/prelude.h"

module clice:unittests.utils.tu_index_mirror;

import :index.format;
import :index.symbol;
import :index.tu_index;
import :unittests.utils;

namespace clice::testing {

/// The TU index blob's serialized layout (tu_index.cpp's TUIndexBlob) through
/// its sections, field for field: tests plant shapes the builder never emits,
/// or re-encode a real TU index with one field changed.
struct TUIndexMirror {
    struct Section {
        std::uint32_t path_id = 0;
        std::uint64_t hash = 0;
        std::vector<std::uint8_t> blob;
    };

    struct InstantiationSection {
        std::uint32_t path_id = 0;
        std::vector<std::uint8_t> blob;
        std::vector<std::uint64_t> skipped;
    };

    std::uint32_t format_version = index::index_format_version;
    std::int64_t built_at = 0;
    std::vector<std::string> paths;
    std::vector<std::uint64_t> content_hashes;
    std::vector<index::IncludeNode> nodes;
    std::vector<std::uint64_t> sym_hashes;
    std::string sym_names;
    std::vector<std::uint32_t> sym_name_ends;
    std::string sym_args;
    std::vector<std::uint32_t> sym_args_ends;
    std::string sym_signatures;
    std::vector<std::uint32_t> sym_signature_ends;
    std::vector<std::uint64_t> sym_parents;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::uint8_t> sym_scopes;
    std::vector<std::uint16_t> sym_flags;
    std::vector<std::uint8_t> sym_token_kinds;
    std::vector<std::uint32_t> sym_token_modifiers;
    std::vector<std::uint32_t> sym_files;
    std::vector<std::uint32_t> sym_reference_ends;
    std::vector<std::uint32_t> sym_references;
    std::vector<std::uint32_t> sym_instantiation_ends;
    std::vector<std::uint32_t> sym_instantiations;
    std::vector<Section> sections;
    std::vector<InstantiationSection> instantiations;

    /// Append a symbol entry; the reader requires ascending hashes.
    void add_symbol(index::SymbolHash hash,
                    const index::SymbolView& identity,
                    llvm::ArrayRef<std::uint32_t> references = {},
                    llvm::ArrayRef<std::uint32_t> instantiation_files = {}) {
        sym_hashes.push_back(hash);
        sym_names += identity.name;
        sym_name_ends.push_back(static_cast<std::uint32_t>(sym_names.size()));
        sym_args += identity.args;
        sym_args_ends.push_back(static_cast<std::uint32_t>(sym_args.size()));
        sym_signatures += identity.signature;
        sym_signature_ends.push_back(static_cast<std::uint32_t>(sym_signatures.size()));
        sym_parents.push_back(identity.parent);
        sym_kinds.push_back(identity.kind.value());
        sym_scopes.push_back(static_cast<std::uint8_t>(identity.scope));
        sym_flags.push_back(static_cast<std::uint16_t>(identity.flags));
        sym_token_kinds.push_back(static_cast<std::uint8_t>(identity.token_class.kind));
        sym_token_modifiers.push_back(identity.token_class.modifiers);
        sym_files.push_back(identity.file);
        sym_references.insert(sym_references.end(), references.begin(), references.end());
        sym_reference_ends.push_back(static_cast<std::uint32_t>(sym_references.size()));
        llvm::append_range(sym_instantiations, instantiation_files);
        sym_instantiation_ends.push_back(static_cast<std::uint32_t>(sym_instantiations.size()));
    }

    /// A loaded TU index's fields through the sections.
    static TUIndexMirror of(const index::TUIndex& view) {
        TUIndexMirror mirror;
        mirror.built_at = view.built_at();
        for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
            mirror.paths.emplace_back(view.path(i));
            mirror.content_hashes.push_back(view.content_hash(i));
        }
        for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
            mirror.nodes.push_back(view.node(i));
        }
        llvm::DenseMap<index::SymbolHash, llvm::ArrayRef<std::uint32_t>> instantiation_files;
        view.iterate_instantiation_files(
            [&](index::SymbolHash hash, llvm::ArrayRef<std::uint32_t> files) {
                instantiation_files[hash] = files;
                return true;
            });
        view.iterate_symbols([&](index::SymbolHash hash,
                                 const index::SymbolView& identity,
                                 llvm::ArrayRef<std::uint32_t> references) {
            mirror.add_symbol(hash, identity, references, instantiation_files.lookup(hash));
            return true;
        });
        for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
            auto blob = view.section_blob(i);
            mirror.sections.push_back({view.section_path(i),
                                       view.section_hash(i),
                                       std::vector<std::uint8_t>(blob.begin(), blob.end())});
        }
        for(std::uint32_t i = 0; i < view.instantiation_count(); i += 1) {
            auto blob = view.instantiation_blob(i);
            auto skipped = view.skipped_instantiations(i);
            mirror.instantiations.push_back({
                view.instantiation_path(i),
                std::vector<std::uint8_t>(blob.begin(), blob.end()),
                {skipped.begin(), skipped.end()}
            });
        }
        return mirror;
    }

    std::string bytes() const {
        auto encoded = kota::codec::fbs::to_bytes(*this);
        ZASSERT(encoded);
        return std::string(encoded->begin(), encoded->end());
    }
};

}  // namespace clice::testing
