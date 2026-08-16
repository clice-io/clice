#include "index/manifest.h"

#include <cstring>
#include <limits>
#include <span>

#include "index/serialization.h"

namespace clice::index {

namespace {

/// The manifest's persisted form. Node and contribution payloads are
/// varint-packed by hand: a big TU enters thousands of files, and LEB128
/// on the small ids and lines roughly halves the blob against fixed-width
/// columns.
struct ManifestBlob {
    std::uint32_t format_version = 0;

    std::uint64_t global_gen = 0;

    std::uint64_t built_at = 0;

    std::uint32_t tu_fv = 0;

    std::uint32_t node_count = 0;

    std::uint32_t contribution_count = 0;

    /// node_count × (varint fv, varint parent + 1 with 0 = root, varint line)
    std::vector<std::uint8_t> nodes;

    /// contribution_count × (varint fv, 8-byte little-endian rows hash —
    /// hashes are random bits, varint would only inflate them)
    std::vector<std::uint8_t> contributions;
};

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
    blob.tu_fv = manifest.tu_fv;

    blob.node_count = static_cast<std::uint32_t>(manifest.nodes.size());
    blob.nodes.reserve(manifest.nodes.size() * 6);
    for(auto& node: manifest.nodes) {
        write_varint(blob.nodes, node.fv);
        write_varint(blob.nodes, node.parent + 1);
        write_varint(blob.nodes, node.line);
    }

    blob.contribution_count = static_cast<std::uint32_t>(manifest.contributions.size());
    blob.contributions.reserve(manifest.contributions.size() * 10);
    for(auto& [fv, hash]: manifest.contributions) {
        write_varint(blob.contributions, fv);
        std::uint8_t bytes[8];
        std::memcpy(bytes, &hash, sizeof(hash));
        blob.contributions.insert(blob.contributions.end(), bytes, bytes + sizeof(bytes));
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
    manifest.tu_fv = blob.tu_fv;

    constexpr std::uint64_t id_max = std::numeric_limits<std::uint32_t>::max();
    std::span<const std::uint8_t> nodes(blob.nodes);
    std::size_t pos = 0;
    manifest.nodes.reserve(blob.node_count);
    for(std::uint32_t i = 0; i < blob.node_count; i += 1) {
        std::uint64_t fv = 0;
        std::uint64_t parent = 0;
        std::uint64_t line = 0;
        if(!read_varint(nodes, pos, fv) || !read_varint(nodes, pos, parent) ||
           !read_varint(nodes, pos, line)) {
            return std::nullopt;
        }
        if(fv > id_max || line > id_max) {
            return std::nullopt;
        }
        // Parents may follow their children (the include graph resolves
        // parent chains after appending the child), so only bounds are
        // checked; consumers walking parents must carry their own visited
        // set.
        if(parent > blob.node_count) {
            return std::nullopt;
        }
        manifest.nodes.push_back({static_cast<std::uint32_t>(fv),
                                  static_cast<std::uint32_t>(parent) - 1,
                                  static_cast<std::uint32_t>(line)});
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
        manifest.contributions.emplace_back(static_cast<std::uint32_t>(fv), hash);
    }
    if(pos != contributions.size()) {
        return std::nullopt;
    }

    return manifest;
}

}  // namespace clice::index
