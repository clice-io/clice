module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.tu_manifest;
import :support.bytes;
import :unittests.utils;

namespace clice::testing {
namespace {

/// A version table of `count` entries, path id and hash apart per entry.
void give_versions(index::TUManifestBlob& layout, std::uint32_t count) {
    for(std::uint32_t i = 0; i < count; i += 1) {
        layout.version_paths.push_back(i);
        layout.version_hashes.push_back(100 + i);
    }
}

ZEST_SUITE(TUManifest) {

ZEST_CASE(ManifestRoundTrip) {
    index::TUManifest manifest;
    manifest.built_at = 1234567;
    manifest.tu_version = VersionID{0};
    manifest.peak = 3ull << 30;
    // A root node, a multi-byte-varint line, a parent that FOLLOWS its child
    // (the include tree resolves parent chains after appending), and runs of
    // siblings — skipped or not, lines ascending or not, under a node or the
    // root.
    manifest.nodes = {
        {0, ~0u, 1,     false},
        {1, 2,   70000, false},
        {2, 0,   12,    false},
        {3, 0,   14,    true },
        {4, 0,   14,    true },
        {5, 0,   3,     true },
        {6, 0,   90000, false},
        {7, ~0u, 5,     true },
        {8, ~0u, 2,     false},
    };
    manifest.instantiations = {
        {.file = VersionID{1}, .use = 0x1234},
        {.file = VersionID{2}, .use = 0x5678},
    };
    manifest.contributions = {
        {VersionID{0}, 0xdeadbeefdeadbeefull},
        {VersionID{2}, 42                   },
    };
    manifest.absent = {VersionID{3}};
    manifest.imports = {VersionID{4}, VersionID{5}};
    manifest.shared_locals = {
        {.symbol = 5, .files = {VersionID{0}, VersionID{2}}},
        {.symbol = 9, .files = {VersionID{2}, VersionID{1}}},
    };
    // Versions 4 and 5 are two versions of one path, read through modules
    // built at different times.
    std::vector<index::PersistedVersion> versions;
    for(std::uint32_t i = 0; i < 9; i += 1) {
        versions.push_back({.path = i == 5 ? 4 : i, .content_hash = 100 + i});
    }

    auto buf = index::serialize_manifest(manifest, versions);

    auto loaded = index::deserialize_manifest(as_chars(buf));
    ZASSERT(loaded);
    ZASSERT(loaded->manifest == manifest);
    ZASSERT(loaded->versions == versions);

    // A version past the table is no manifest.
    versions.pop_back();
    ZASSERT(!index::deserialize_manifest(as_chars(index::serialize_manifest(manifest, versions))));
}

ZEST_CASE(ManifestJunkRejected) {
    ZASSERT(!index::deserialize_manifest("not a blob").has_value());
}

ZEST_CASE(ManifestSharedLocalRejected) {
    index::TUManifestBlob valid;
    valid.format_version = index::index_format_version;
    valid.contribution_count = 1;
    valid.contributions = {1, 0, 0, 0, 0, 0, 0, 0, 0};
    valid.local_symbols = {5, 7};
    valid.local_file_ends = {1, 2};
    valid.local_files = {0, 0};
    give_versions(valid, 2);

    auto decodes = [&](const index::TUManifestBlob& layout) {
        return index::deserialize_manifest(as_chars(index::encode(layout))).has_value();
    };
    ZASSERT(decodes(valid));

    auto layout = valid;
    layout.local_symbols = {7, 5};
    ZASSERT(!decodes(layout));

    layout = valid;
    layout.local_file_ends = {1};
    ZASSERT(!decodes(layout));

    layout = valid;
    layout.local_file_ends = {1, 1};
    ZASSERT(!decodes(layout));

    // The symbols key a DenseMap at load.
    layout = valid;
    layout.local_symbols = {5, ~0ull};
    ZASSERT(!decodes(layout));
}

ZEST_CASE(ManifestCorruptUsesRejected) {
    // A use the manifest names keys a DenseMap at load: a sentinel value
    // must not decode, nor a use no file lists.
    index::TUManifestBlob layout;
    layout.format_version = index::index_format_version;
    layout.instantiation_files = {1};
    layout.instantiation_uses = {42};
    give_versions(layout, 2);
    auto decodes = [&] {
        return index::deserialize_manifest(as_chars(index::encode(layout))).has_value();
    };
    ZASSERT(decodes());
    layout.instantiation_uses = {~0ull};
    ZEXPECT(!decodes());
    layout.instantiation_uses = {~0ull - 1};
    ZEXPECT(!decodes());
    layout.instantiation_files = {};
    layout.instantiation_uses = {42};
    ZEXPECT(!decodes());
}

ZEST_CASE(ManifestNodeCodesRejected) {
    // A node can stand beside the previous node's parent only after one, and
    // only beside it can its line be a distance.
    index::TUManifestBlob layout;
    layout.format_version = index::index_format_version;
    layout.node_count = 1;
    give_versions(layout, 8);
    auto decodes = [&] {
        return index::deserialize_manifest(as_chars(index::encode(layout))).has_value();
    };
    layout.nodes = {7, 1, 5 << 2};
    ZASSERT(decodes());
    layout.nodes = {7, 0, 5 << 2};
    ZEXPECT(!decodes());
    layout.nodes = {7, 1, (5 << 2) | 1};
    ZEXPECT(!decodes());
    layout.nodes = {7, 3, 5 << 2};
    ZEXPECT(!decodes());
}

ZEST_CASE(ManifestCountMismatchRejected) {
    // A node count reporting more nodes than the payload holds must not
    // decode.
    index::TUManifestBlob layout;
    layout.format_version = index::index_format_version;
    layout.node_count = 2;
    layout.nodes = {1, 0, 5};  // one node's worth of varints

    ZASSERT(!index::deserialize_manifest(as_chars(index::encode(layout))).has_value());
}

ZEST_CASE(ManifestVarintOverflowRejected) {
    // A ten-byte varint whose last byte carries more than value bit 63
    // would silently shift the excess out and decode to an unrelated small
    // id, redirecting contributions to another file.
    index::TUManifestBlob layout;
    layout.format_version = index::index_format_version;
    layout.node_count = 1;
    layout.nodes = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0x00, 0x00};

    ZASSERT(!index::deserialize_manifest(as_chars(index::encode(layout))).has_value());
}

};  // ZEST_SUITE(TUManifest)

}  // namespace
}  // namespace clice::testing
