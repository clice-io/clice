module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.tu_manifest;

namespace clice::index {

namespace {

void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while(value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value) | 0x80);
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

bool read_varint(std::span<const std::uint8_t> data, std::size_t& pos, std::uint64_t& value) {
    value = 0;
    for(std::uint32_t shift = 0; shift < 64; shift += 7) {
        if(pos >= data.size()) {
            return false;
        }
        auto byte = data[pos];
        pos += 1;
        // The tenth byte holds only value bit 63; greater payloads would
        // shift out silently and decode to an unrelated small value.
        if(shift == 63 && byte > 1) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void serialize_manifest(const TUManifest& manifest, llvm::raw_ostream& os) {
    ManifestBlob blob;
    blob.format_version = index_format_version;
    blob.global_gen = manifest.global_gen;
    blob.built_at = manifest.built_at;
    blob.tu_fv = manifest.tu_fv.raw;

    blob.node_count = static_cast<std::uint32_t>(manifest.nodes.size());
    blob.nodes.reserve(manifest.nodes.size() * 5);
    const IncludeNode* previous = nullptr;
    for(auto& node: manifest.nodes) {
        write_varint(blob.nodes, node.file);
        bool sibling = previous && previous->parent == node.parent;
        write_varint(blob.nodes, sibling ? 0 : node.parent == no_node ? 1 : node.parent + 2);
        bool delta = sibling && node.line >= previous->line;
        auto line = delta ? node.line - previous->line : node.line;
        write_varint(blob.nodes,
                     (std::uint64_t(line) << 2) | (node.skipped ? 2 : 0) | (delta ? 1 : 0));
        previous = &node;
    }

    blob.contribution_count = static_cast<std::uint32_t>(manifest.contributions.size());
    blob.contributions.reserve(manifest.contributions.size() * 10);
    for(auto& [fv, hash]: manifest.contributions) {
        write_varint(blob.contributions, fv.raw);
        std::uint8_t bytes[8];
        std::memcpy(bytes, &hash, sizeof(hash));
        blob.contributions.insert(blob.contributions.end(), bytes, bytes + sizeof(bytes));
    }

    for(auto fv: manifest.absent) {
        blob.absent.push_back(fv.raw);
    }
    for(auto fv: manifest.imports) {
        blob.imports.push_back(fv.raw);
    }

    for(auto& local: manifest.shared_locals) {
        blob.local_symbols.push_back(local.symbol);
        for(auto fv: local.files) {
            blob.local_files.push_back(fv.raw);
        }
        blob.local_file_ends.push_back(static_cast<std::uint32_t>(blob.local_files.size()));
    }

    for(auto& use: manifest.instantiations) {
        blob.instantiation_files.push_back(use.file.raw);
        blob.instantiation_uses.push_back(use.use);
    }

    serialize_blob(blob, os);
}

std::optional<TUManifest> deserialize_manifest(llvm::StringRef data) {
    ManifestBlob blob;
    if(!deserialize_blob(data, blob) || blob.format_version != index_format_version) {
        return std::nullopt;
    }

    // The counts size reserves below and are untrusted; a node occupies at
    // least 3 payload bytes (three varints) and a contribution at least 9
    // (varint + 8-byte hash), so a count beyond these bounds cannot be
    // honest and must not reach an allocator.
    if(blob.node_count > blob.nodes.size() / 3 ||
       blob.contribution_count > blob.contributions.size() / 9) {
        return std::nullopt;
    }

    TUManifest manifest;
    manifest.global_gen = blob.global_gen;
    manifest.built_at = blob.built_at;
    manifest.tu_fv = VersionID{blob.tu_fv};

    constexpr std::uint64_t id_max = std::numeric_limits<std::uint32_t>::max();
    std::span<const std::uint8_t> nodes(blob.nodes);
    std::size_t pos = 0;
    manifest.nodes.reserve(blob.node_count);
    for(std::uint32_t i = 0; i < blob.node_count; i += 1) {
        std::uint64_t fv = 0;
        std::uint64_t parent = 0;
        std::uint64_t code = 0;
        if(!read_varint(nodes, pos, fv) || !read_varint(nodes, pos, parent) ||
           !read_varint(nodes, pos, code)) {
            return std::nullopt;
        }
        // Parents may follow their children (the include tree resolves
        // parent chains after appending the child), so only bounds are
        // checked; consumers walking parents must carry their own visited
        // set.
        auto* previous = manifest.nodes.empty() ? nullptr : &manifest.nodes.back();
        bool delta = (code & 1) != 0;
        if(fv > id_max || parent > std::uint64_t(blob.node_count) + 1 ||
           (parent == 0 && !previous) || (delta && parent != 0)) {
            return std::nullopt;
        }
        auto line = (code >> 2) + (delta ? previous->line : 0);
        if(line > id_max) {
            return std::nullopt;
        }
        manifest.nodes.push_back({
            .file = static_cast<std::uint32_t>(fv),
            .parent = parent == 0   ? previous->parent
                      : parent == 1 ? no_node
                                    : static_cast<std::uint32_t>(parent - 2),
            .line = static_cast<std::uint32_t>(line),
            .skipped = (code & 2) != 0,
        });
    }
    if(pos != nodes.size()) {
        return std::nullopt;
    }

    std::span<const std::uint8_t> contributions(blob.contributions);
    pos = 0;
    manifest.contributions.reserve(blob.contribution_count);
    for(std::uint32_t i = 0; i < blob.contribution_count; i += 1) {
        std::uint64_t fv = 0;
        if(!read_varint(contributions, pos, fv) || fv > id_max) {
            return std::nullopt;
        }
        if(pos + 8 > contributions.size()) {
            return std::nullopt;
        }
        std::uint64_t hash = 0;
        std::memcpy(&hash, contributions.data() + pos, sizeof(hash));
        pos += 8;
        manifest.contributions.emplace_back(VersionID{static_cast<std::uint32_t>(fv)}, hash);
    }
    if(pos != contributions.size()) {
        return std::nullopt;
    }

    for(auto fv: blob.absent) {
        manifest.absent.push_back(VersionID{fv});
    }
    for(auto fv: blob.imports) {
        manifest.imports.push_back(VersionID{fv});
    }

    if(blob.local_file_ends.size() != blob.local_symbols.size() ||
       !covering_ends(blob.local_file_ends, blob.local_files.size())) {
        return std::nullopt;
    }
    std::uint32_t begin = 0;
    for(std::size_t i = 0; i < blob.local_symbols.size(); i += 1) {
        if(reserved_key(blob.local_symbols[i]) ||
           (i > 0 && blob.local_symbols[i] <= blob.local_symbols[i - 1])) {
            return std::nullopt;
        }
        auto end = blob.local_file_ends[i];
        auto& local = manifest.shared_locals.emplace_back();
        local.symbol = blob.local_symbols[i];
        for(auto fv: llvm::ArrayRef(blob.local_files).slice(begin, end - begin)) {
            local.files.push_back(VersionID{fv});
        }
        begin = end;
    }

    if(blob.instantiation_uses.size() != blob.instantiation_files.size() ||
       llvm::any_of(blob.instantiation_uses, [](std::uint64_t use) { return reserved_key(use); })) {
        return std::nullopt;
    }
    for(std::size_t i = 0; i < blob.instantiation_files.size(); i += 1) {
        manifest.instantiations.push_back({
            .file = VersionID{blob.instantiation_files[i]},
            .use = blob.instantiation_uses[i],
        });
    }

    return manifest;
}

void measure_manifest(llvm::StringRef data, FieldBytes& bytes) {
    if(ManifestBlob blob; deserialize_blob(data, blob)) {
        measure_fields(blob, bytes);
    }
}

}  // namespace clice::index
