#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "index/include_tree.h"
#include "index/serialization.h"
#include "index/tu_index.h"
#include "index/types.h"

#include "llvm/ADT/ArrayRef.h"

namespace clice::testing {

/// The envelope's wire layout (tu_index.cpp's EnvelopeBlob) through its
/// sections, field for field: tests plant shapes the builder never emits,
/// or re-encode a real envelope with one field changed.
struct EnvelopeMirror {
    struct Section {
        std::uint32_t path_id = 0;
        std::uint64_t hash = 0;
        std::vector<std::uint8_t> blob;
    };

    std::uint32_t format_version = index::index_format_version;
    std::int64_t built_at = 0;
    std::vector<std::string> paths;
    std::vector<std::uint64_t> path_hashes;
    std::vector<index::IncludeNode> nodes;
    std::vector<std::uint64_t> sym_hashes;
    std::string sym_names;
    std::vector<std::uint32_t> sym_name_ends;
    std::string sym_args;
    std::vector<std::uint32_t> sym_args_ends;
    std::vector<std::uint64_t> sym_parents;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::uint8_t> sym_scopes;
    std::vector<std::uint16_t> sym_flags;
    std::vector<std::uint32_t> sym_files;
    std::vector<std::uint32_t> sym_reference_ends;
    std::vector<std::uint32_t> sym_references;
    std::vector<Section> sections;

    /// Append a symbol row; the reader requires ascending hashes.
    void add_symbol(index::SymbolHash hash,
                    const index::SymbolIdentity& identity,
                    llvm::ArrayRef<std::uint32_t> references = {}) {
        sym_hashes.push_back(hash);
        sym_names += identity.name;
        sym_name_ends.push_back(static_cast<std::uint32_t>(sym_names.size()));
        sym_args += identity.args;
        sym_args_ends.push_back(static_cast<std::uint32_t>(sym_args.size()));
        sym_parents.push_back(identity.parent);
        sym_kinds.push_back(identity.kind.value());
        sym_scopes.push_back(static_cast<std::uint8_t>(identity.scope));
        sym_flags.push_back(static_cast<std::uint16_t>(identity.flags));
        sym_files.push_back(identity.file);
        sym_references.insert(sym_references.end(), references.begin(), references.end());
        sym_reference_ends.push_back(static_cast<std::uint32_t>(sym_references.size()));
    }

    /// Every field of a loaded envelope.
    static EnvelopeMirror of(const index::TUIndex& view) {
        EnvelopeMirror mirror;
        mirror.built_at = view.built_at();
        for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
            mirror.paths.emplace_back(view.path(i));
            mirror.path_hashes.push_back(view.path_hash(i));
        }
        for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
            mirror.nodes.push_back(view.node(i));
        }
        view.iterate_symbols([&](index::SymbolHash hash,
                                 const index::SymbolIdentity& identity,
                                 llvm::ArrayRef<std::uint32_t> references) {
            mirror.add_symbol(hash, identity, references);
            return true;
        });
        for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
            auto blob = view.section_blob(i);
            mirror.sections.push_back({view.section_path(i),
                                       view.section_hash(i),
                                       std::vector<std::uint8_t>(blob.begin(), blob.end())});
        }
        return mirror;
    }

    std::string bytes() const {
        auto encoded = kota::codec::fbs::to_bytes(*this);
        if(!encoded) {
            return {};
        }
        return std::string(encoded->begin(), encoded->end());
    }
};

}  // namespace clice::testing
