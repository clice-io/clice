module clice;

import :index.database;
import :index.project_index;
import :index.project_index.manifest;
import :index.project_index.records;
import :support.logging;

namespace clice::index {

namespace {

/// What one unit's manifest says about one file, in path ids.
struct UnitFacts {
    /// The content hashes the unit entered the file at.
    llvm::SmallVector<std::uint64_t, 1> read;

    llvm::SmallVector<VariantHash, 1> variants;

    bool absent = false;

    /// The content hashes the unit read the file at through a module or a
    /// precompiled header.
    llvm::SmallVector<std::uint64_t, 1> imported;

    llvm::SmallVector<UseHash, 1> uses;

    /// Per content hash, the unit's resolution of the file's own
    /// `#include` lines.
    llvm::SmallVector<
        std::pair<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>>,
        1>
        resolutions;

    friend bool operator==(const UnitFacts&, const UnitFacts&) = default;
};

template <typename Range>
void add_distinct(Range& range, std::uint64_t value) {
    if(!ranges::contains(range, value)) {
        range.push_back(value);
    }
}

}  // namespace

bool ProjectIndex::indexed(Fid tu) const {
    auto* record = file_record(tu);
    return record && record->run != 0;
}

std::uint64_t ProjectIndex::indexed_command(Fid tu) const {
    auto* record = file_record(tu);
    return record ? record->command : 0;
}

std::optional<TUManifest> ProjectIndex::manifest(Fid tu) const {
    auto key = file_key(tu);
    for(auto* held: {&manifest_records.changed, &manifest_records.written}) {
        if(auto it = held->find(key); it != held->end()) {
            return it->second;
        }
    }
    if(!db || !indexed(tu)) {
        return std::nullopt;
    }
    auto blob = db->read(BlobKind::TUManifest, key);
    auto decoded = blob ? decode_manifest(blob.buffer->getBuffer()) : std::nullopt;
    if(!decoded) {
        damaged_record(BlobKind::TUManifest, key);
    }
    return decoded;
}

std::optional<TUManifest> ProjectIndex::decode_manifest(llvm::StringRef data) const {
    auto persisted = deserialize_manifest(data);
    if(!persisted) {
        return std::nullopt;
    }
    llvm::SmallVector<VersionID> versions;
    for(auto id: persisted->versions) {
        auto version = version_of(id);
        if(!version.valid()) {
            return std::nullopt;
        }
        versions.push_back(version);
    }
    auto& manifest = persisted->manifest;
    for_each_version(manifest, [&](std::uint32_t& id) { id = versions[id].raw; });
    return std::move(manifest);
}

std::vector<std::byte> ProjectIndex::encode_manifest(const TUManifest& manifest) const {
    std::vector<std::uint32_t> ids;
    for_each_version(manifest, [&](std::uint32_t version) {
        auto it = version_ids.find(VersionID{version});
        assert(it != version_ids.end() && "an installed manifest's versions are named");
        ids.push_back(it->second);
    });
    ranges::sort(ids);
    ids.erase(ranges::unique(ids).begin(), ids.end());
    auto persisted = manifest;
    for_each_version(persisted, [&](std::uint32_t& version) {
        auto id = version_ids.find(VersionID{version})->second;
        version = static_cast<std::uint32_t>(ranges::lower_bound(ids, id) - ids.begin());
    });
    return serialize_manifest(persisted, ids);
}

llvm::SmallVector<Fid> ProjectIndex::apply_manifest(Fid tu, TUManifest manifest) {
    // The run is what tells a unit with a manifest (FileRecord::run).
    assert(manifest.built_at != 0 && "a manifest names the run that built it");
    assert(manifest.tu_version.valid() && "a manifest names the unit's version");
    ranges::sort(manifest.absent);
    manifest.absent.erase(ranges::unique(manifest.absent).begin(), manifest.absent.end());
    return replace_manifest(tu, std::move(manifest));
}

llvm::SmallVector<Fid> ProjectIndex::remove_manifest(Fid tu) {
    if(!indexed(tu)) {
        return {};
    }
    return replace_manifest(tu, std::nullopt);
}

llvm::SmallVector<Fid> ProjectIndex::replace_manifest(Fid tu, std::optional<TUManifest> manifest) {
    manifests_changed += 1;
    // A new manifest stores what the unit contributes; none owes nothing.
    header_changed = owed.erase(tu) || header_changed;
    auto unit = path_id(tu);
    auto old = this->manifest(tu);

    auto path_of = [&](VersionID version) {
        return path_id(files.version(version).fid);
    };
    auto facts_of = [&](const TUManifest& of) {
        llvm::DenseMap<Fid, UnitFacts> facts;
        auto entered = [&](VersionID version) {
            auto& record = files.version(version);
            add_distinct(facts[record.fid].read, record.content_hash);
        };
        entered(of.tu_version);
        for(auto& node: of.nodes) {
            entered(VersionID{node.file});
            auto parent =
                node.parent == no_node ? of.tu_version : VersionID{of.nodes[node.parent].file};
            auto& record = files.version(parent);
            auto& resolutions = facts[record.fid].resolutions;
            auto it = ranges::find_if(resolutions, [&](const auto& resolution) {
                return resolution.first == record.content_hash;
            });
            if(it == resolutions.end()) {
                resolutions.emplace_back(record.content_hash,
                                         std::vector<std::pair<std::uint32_t, std::uint32_t>>());
                it = std::prev(resolutions.end());
            }
            it->second.emplace_back(node.line, path_of(VersionID{node.file}));
        }
        for(auto& [version, variant]: of.contributions) {
            auto& record = files.version(version);
            add_distinct(facts[record.fid].read, record.content_hash);
            add_distinct(facts[record.fid].variants, variant);
        }
        for(auto version: of.absent) {
            facts[files.version(version).fid].absent = true;
        }
        for(auto version: of.imports) {
            auto& record = files.version(version);
            add_distinct(facts[record.fid].imported, record.content_hash);
        }
        for(auto& use: of.instantiations) {
            add_distinct(facts[files.version(use.file).fid].uses, use.use);
        }
        for(auto& file:
            views::transform(facts, [](auto& entry) -> auto& { return entry.second; })) {
            ranges::sort(file.read);
            ranges::sort(file.variants);
            ranges::sort(file.imported);
            ranges::sort(file.uses);
            for(auto& edges: views::values(file.resolutions)) {
                ranges::sort(edges);
                edges.erase(ranges::unique(edges).begin(), edges.end());
            }
            ranges::sort(file.resolutions);
        }
        return facts;
    };
    auto old_facts = old ? facts_of(*old) : llvm::DenseMap<Fid, UnitFacts>();
    auto new_facts = manifest ? facts_of(*manifest) : llvm::DenseMap<Fid, UnitFacts>();

    llvm::SmallVector<Fid> affected;
    auto update = [&](Fid file, const UnitFacts& before, const UnitFacts& after) {
        if(before == after) {
            return;
        }
        auto& record = change_dependents(file);
        for(auto hash: before.read) {
            remove_unit(record.readers, hash, unit);
        }
        for(auto variant: before.variants) {
            remove_unit(record.contributors, variant, unit);
        }
        if(before.absent) {
            record.absent.remove(unit);
        }
        for(auto hash: before.imported) {
            remove_unit(record.importers, hash, unit);
        }
        for(auto use: before.uses) {
            remove_unit(record.uses, use, unit);
        }
        for(auto& [content, edges]: before.resolutions) {
            auto it = ranges::find_if(record.resolutions, [&](const IncludeResolution& resolution) {
                return resolution.content == content && resolution.edges == edges;
            });
            if(it != record.resolutions.end()) {
                it->units.remove(unit);
                if(it->units.isEmpty()) {
                    record.resolutions.erase(it);
                }
            }
        }
        for(auto hash: after.read) {
            add_unit(record.readers, hash, unit);
        }
        for(auto variant: after.variants) {
            add_unit(record.contributors, variant, unit);
        }
        if(after.absent) {
            record.absent.add(unit);
        }
        for(auto hash: after.imported) {
            add_unit(record.importers, hash, unit);
        }
        for(auto use: after.uses) {
            add_unit(record.uses, use, unit);
        }
        for(auto& [content, edges]: after.resolutions) {
            auto it = ranges::find_if(record.resolutions, [&](const IncludeResolution& resolution) {
                return resolution.content == content && resolution.edges == edges;
            });
            if(it == record.resolutions.end()) {
                record.resolutions.push_back({.content = content, .edges = edges});
                it = std::prev(record.resolutions.end());
            }
            it->units.add(unit);
        }

        auto& file_entry = change_file(file);
        file_entry.consumed.clear();
        for(auto& keyed: {&record.readers, &record.importers}) {
            for(auto hash: views::keys(*keyed)) {
                file_entry.consumed.push_back(hash);
            }
        }
        ranges::sort(file_entry.consumed);
        file_entry.consumed.erase(ranges::unique(file_entry.consumed).begin(),
                                  file_entry.consumed.end());
        file_entry.absent = !record.absent.isEmpty();

        if(before.variants != after.variants || before.uses != after.uses ||
           before.resolutions != after.resolutions) {
            affected.push_back(file);
        }
    };
    const UnitFacts none{};
    for(auto& [file, facts]: new_facts) {
        auto it = old_facts.find(file);
        update(file, it != old_facts.end() ? it->second : none, facts);
    }
    for(auto& [file, facts]: old_facts) {
        if(!new_facts.contains(file)) {
            update(file, facts, none);
        }
    }

    // The unit's previous run walked instantiation keys in the files of its
    // old manifest: the next save drops them.
    if(old) {
        for(auto& use: old->instantiations) {
            compact.insert(files.version(use.file).fid);
        }
    }
    auto versions_of = [&](const std::optional<TUManifest>& of) {
        llvm::DenseSet<VersionID> versions;
        if(of) {
            for_each_version(*of,
                             [&](std::uint32_t version) { versions.insert(VersionID{version}); });
        }
        return versions;
    };
    auto old_versions = versions_of(old);
    auto new_versions = versions_of(manifest);
    for(auto version: new_versions) {
        if(!old_versions.contains(version)) {
            retain_version(version);
        }
    }
    for(auto version: old_versions) {
        if(!new_versions.contains(version)) {
            release_version(version);
        }
    }

    auto& record = change_file(tu);
    record.run = manifest ? manifest->built_at : 0;
    record.command = manifest ? manifest->command : 0;
    manifest_records.changed[file_key(tu)] = std::move(manifest);

    ranges::sort(affected);
    for(auto file: affected) {
        refresh_live(file);
    }
    return affected;
}

}  // namespace clice::index
