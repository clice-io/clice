module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.instantiation_shard;
import :index.project_index;
import :index.project_table;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
import :support.logging;
import :vfs.disk_state;
import :vfs.file_system;
import :vfs.file_table;

namespace clice::index {

std::expected<IndexReport, MergeError> ProjectIndex::merge(llvm::StringRef tu_index,
                                                           llvm::ArrayRef<DepState> imports) {
    // Zero-copy consumption: the result stays serialized; a new variant's
    // blob bytes are sliced out and installed or merged without decoding
    // the TU index, and only genuinely new symbol names are materialized.
    auto view = TUIndex::from_bytes(tu_index);
    if(!view.loaded()) {
        LOG_WARN("Ignoring TUIndex that failed verification");
        return std::unexpected(MergeError::Invalid);
    }
    auto focus_local_id = view.path_count() - 1;
    llvm::StringRef tu_path = view.path(focus_local_id);

    // Interning paths only names them — pool ids left behind by a rejected
    // result are inert. Everything that is index STATE (symbols,
    // FileVersions, the manifest, shards) commits only below the section
    // loop, once every part of the result validated.
    llvm::SmallVector<Fid> file_ids_map;
    file_ids_map.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map[i] = files.intern(Spelling::absolute(view.path(i)));
    }
    auto tu_path_id = file_ids_map[focus_local_id];

    TUManifest manifest;
    manifest.built_at = static_cast<std::uint64_t>(view.built_at());

    std::uint32_t hits = 0;
    std::uint32_t appended = 0;
    // Staged, not committed: a section that fails verification rejects the
    // whole result mid-loop, and shards installed before that point would
    // leave the surviving manifest referencing variants the new blobs no
    // longer store.
    llvm::SmallVector<std::pair<Fid, Shard>> replacements;
    // (TU-local path id, variant identity) per serving section; the
    // FileVersions these will reference are interned only at commit.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint64_t>> section_contributions;
    llvm::SmallVector<Fid> rebuilt_ids;
    // TU-local path id -> content hash of the bytes each section's index
    // was built from (0 = no section). A section's shard already records
    // that hash, so the FileVersion baseline below adopts it: pairing the
    // index with any other hash — the file behind a PCM whose disk moved
    // on under a preserved or backdated mtime — would keep the baseline
    // fresh while queries serve another generation's index.
    llvm::SmallVector<std::uint64_t> consumed_hashes(view.path_count(), 0);
    auto record_consumed = [&](std::uint32_t local_id, std::uint64_t content_hash) {
        auto compiled_hash = view.content_hash(local_id);
        if(compiled_hash != 0 && compiled_hash != content_hash) {
            LOG_WARN(
                "Reject merge for {}: the entries for {} consumed other content than the compiler",
                tu_path,
                files.resolve(file_ids_map[local_id]));
            return false;
        }
        consumed_hashes[local_id] = content_hash;
        return true;
    };
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto blob_hash = view.section_hash(section);
        auto global_id = file_ids_map[local_id];

        // Fast path: the blob stores this variant, as the held shard says,
        // else as the units' record does, which a blob is verified against
        // when read (known_variants offers the same). The identity hashes
        // the blob bytes, which embed the content generation, so membership
        // is the whole check — no IO, no bytes read.
        auto held = shards.find(global_id);
        std::optional<std::uint64_t> generation;
        if(held == shards.end()) {
            generation = variant_generation(global_id, blob_hash);
        } else if(held->second.loaded() && held->second.has_variant(blob_hash)) {
            generation = held->second.content_hash();
        }
        if(generation) {
            if(!record_consumed(local_id, *generation)) {
                return std::unexpected(MergeError::Invalid);
            }
            section_contributions.emplace_back(local_id, blob_hash);
            hits += 1;
            continue;
        }

        auto bytes = view.section_blob(section);
        if(bytes.empty()) {
            LOG_INFO("Rerun {}: {} stores no variant of the hash it names",
                     tu_path,
                     files.resolve(global_id));
            return std::unexpected(MergeError::Outdated);
        }

        // The recomputed hash guards the variant identity alongside the
        // structural verification below: bytes installed under a hash they
        // do not reproduce would satisfy every later hit-path check for
        // that hash while the shard stores different entries. A mismatch (or
        // an invalid blob) leaves every recorded version matching the
        // disk, so an installed manifest would be judged fresh forever
        // with this file's index missing or stale — reject the whole
        // result; nothing is committed yet.
        if(llvm::xxh3_64bits(bytes) != blob_hash) {
            LOG_WARN("Reject merge for {}: the section for {} failed verification",
                     tu_path,
                     files.resolve(global_id));
            return std::unexpected(MergeError::Invalid);
        }
        auto fresh = Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(!fresh.loaded()) {
            LOG_WARN("Reject merge for {}: the entries for {} do not form a valid shard",
                     tu_path,
                     files.resolve(global_id));
            return std::unexpected(MergeError::Invalid);
        }
        if(!record_consumed(local_id, fresh.content_hash())) {
            return std::unexpected(MergeError::Invalid);
        }

        Shard replacement;
        auto* shard = this->shard(global_id);
        if(shard && shard->content_hash() == fresh.content_hash()) {
            // Same generation, new variant: merge it in, keeping every
            // stored variant — dead ones stay masked until the next save
            // compacts them.
            auto merged = merge_shards(*shard, shard->variants(), llvm::ArrayRef(fresh));
            replacement = Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(merged));
            assert(replacement.loaded() && "a freshly merged shard blob must verify");
            appended += 1;
        } else {
            // New content generation (or no blob at all): entries from other
            // generations must never share offset storage with these, so
            // the worker's bytes become the blob verbatim. Stale
            // contributions from other TUs stop matching any stored
            // variant; the commit re-enqueues their owners.
            replacement = std::move(fresh);
            rebuilt_ids.push_back(global_id);
        }
        replacements.emplace_back(global_id, std::move(replacement));
        section_contributions.emplace_back(local_id, blob_hash);
    }

    // Instantiations: a skipped key stands for the resolution its file stores
    // for it, a walked one merges in, and what the unit uses there is one use
    // of the file's blob. Staged like the shards.
    Producer producer{
        .unit = unit_hash(tu_path_id),
        .run = static_cast<std::uint64_t>(view.built_at()),
    };
    llvm::SmallVector<std::pair<Fid, InstantiationShard>> instantiation_replacements;
    llvm::DenseMap<Fid, std::pair<std::uint32_t, UseHash>> unit_uses;
    for(std::uint32_t section = 0; section < view.instantiation_count(); section += 1) {
        auto local_id = view.instantiation_path(section);
        auto global_id = file_ids_map[local_id];
        auto generation = consumed_hashes[local_id] != 0 ? consumed_hashes[local_id]
                                                         : view.content_hash(local_id);
        const InstantiationShard* stored = instantiation_shard(global_id);
        if(stored && stored->content_hash() != generation) {
            stored = nullptr;
        }

        std::vector<ResolutionHash> used;
        for(auto key: view.skipped_instantiations(section)) {
            auto found = stored ? stored->find_key(key) : std::nullopt;
            if(!found) {
                LOG_INFO("Rerun {}: {} stores no instantiation it skipped",
                         tu_path,
                         files.resolve(global_id));
                return std::unexpected(MergeError::Outdated);
            }
            if(found->resolution) {
                used.push_back(*found->resolution);
            }
        }

        auto bytes = view.instantiation_blob(section);
        std::optional<InstantiationShard> fresh;
        if(!bytes.empty()) {
            fresh = InstantiationShard::from_bytes(bytes);
            if(!fresh->loaded()) {
                LOG_WARN("Reject merge for {}: instantiations for {} do not form a valid blob",
                         tu_path,
                         files.resolve(global_id));
                return std::unexpected(MergeError::Invalid);
            }
            if(!record_consumed(local_id, fresh->content_hash())) {
                return std::unexpected(MergeError::Invalid);
            }
            llvm::append_range(used, fresh->resolutions());
        }
        llvm::sort(used);
        used.erase(llvm::unique(used), used.end());
        std::optional<AddedUse> added;
        if(!used.empty()) {
            added = AddedUse{.use = use_hash(used), .resolutions = used};
            unit_uses[global_id] = {local_id, added->use};
        }
        // Walking nothing new, a unit naming a use the file stores already
        // changes nothing there.
        if(!fresh && (!added || stored->has_use(added->use))) {
            continue;
        }
        const static InstantiationShard none;
        const auto& old = stored ? *stored : none;
        auto merged = merge_instantiations(
            old,
            old.uses(),
            [&](const Producer& run) { return current_run(run); },
            fresh ? &*fresh : nullptr,
            producer,
            added);
        auto replacement = InstantiationShard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(merged.value_or("")));
        assert(replacement.loaded() && "a freshly merged instantiation blob must verify");
        instantiation_replacements.emplace_back(global_id, std::move(replacement));
    }

    // The files the unit holds entries of: its own sections, the
    // resolutions it uses, and what it reads through the precompiled header
    // or its modules, whose own units wrote the entries there.
    llvm::BitVector held(view.path_count());
    for(auto local_id: llvm::make_first_range(section_contributions)) {
        held.set(local_id);
    }
    for(auto local_id: llvm::make_first_range(llvm::make_second_range(unit_uses))) {
        held.set(local_id);
    }
    llvm::DenseSet<Fid> read_precompiled;
    for(auto& dep: imports) {
        read_precompiled.insert(dep.path_id);
    }
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        if(read_precompiled.contains(file_ids_map[i])) {
            held.set(i);
        }
    }
    auto shared_locals = view.shared_locals(held);
    if(!shared_locals) {
        LOG_WARN(
            "Reject merge for {}: a symbol below external scope names a file the unit holds "
            "no entries of",
            tu_path);
        return std::unexpected(MergeError::Invalid);
    }

    // The first commit.
    llvm::SmallVector<SymbolHash> added;
    merge_symbols(view, file_ids_map, &added);
    pending_symbols.insert(added.begin(), added.end());
    if(changed_during_search_build) {
        changed_during_search_build->symbols.insert(added.begin(), added.end());
    }
    merges_since_search_build += 1;

    // Intern a FileVersion per file of the parse: the consumed-content hash
    // from the compiler's own buffers, shared by every TU that consumed it
    // (see file_version_stale).
    auto baseline_before_ns = vfs::stat_baseline_before_ns(view.built_at());
    llvm::SmallVector<VersionID> version_of;
    version_of.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        llvm::StringRef path = view.path(i);
        // The section's own record wins: for a hashless path (behind a
        // PCM) it is the only hash naming the bytes the index describes.
        auto hash = consumed_hashes[i] != 0 ? consumed_hashes[i] : view.content_hash(i);

        // The worker had no buffer to hash a file behind a PCM, and no
        // shard recorded one.
        if(hash != 0) {
            files.disk.consumed(file_ids_map[i], hash);
        } else if(auto status = vfs::status(path)) {
            hash = files.disk.unchanged_hash(file_ids_map[i], *status, baseline_before_ns);
        }

        version_of[i] = files.intern_version(file_ids_map[i], hash);
    }

    manifest.tu_version = version_of[focus_local_id];
    manifest.nodes.reserve(view.node_count());
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        auto node = view.node(i);
        manifest.nodes.push_back({.file = version_of[node.file].raw,
                                  .parent = node.parent,
                                  .line = node.line,
                                  .skipped = node.skipped});
    }
    for(auto [local_id, variant]: section_contributions) {
        manifest.contributions.emplace_back(version_of[local_id], variant);
    }
    for(auto& local: *shared_locals) {
        SharedLocal entry{.symbol = local.symbol};
        for(auto path_id: local.paths) {
            entry.files.push_back(version_of[path_id]);
        }
        manifest.shared_locals.push_back(std::move(entry));
    }
    for(auto [local_id, use]: llvm::make_second_range(unit_uses)) {
        manifest.instantiations.push_back({.file = version_of[local_id], .use = use});
    }

    // The places the parse's failed lookups looked: the file table watches
    // them from here on. One that holds a file by now makes the index stale
    // on arrival.
    IndexReport report;
    for(std::uint32_t i = 0; i < view.absent_count(); i += 1) {
        auto fid = files.intern(Spelling::absolute(view.absent(i)));
        if(files.disk.current(fid)) {
            report.add_reindex(tu_path_id);
        }
        manifest.absent.push_back(files.intern_version(fid, 0));
    }

    // The inputs of the imported modules and of the precompiled header
    // never enter the parse's include tree, yet each is an input of the
    // parse all the same. Not stale on arrival like the parse's own
    // lookups: a reindex would read the same PCM, which only a change
    // event reaching the module rebuilds. An input the module's build
    // could not version reads as stale until a rebuild names its bytes.
    for(auto& dep: imports) {
        if(dep.missing) {
            manifest.absent.push_back(files.intern_version(dep.path_id, 0));
        } else {
            manifest.imports.push_back(dep.version.valid() ? dep.version
                                                           : files.intern_version(dep.path_id, 0));
        }
    }

    for(auto& [global_id, replacement]: replacements) {
        shards[global_id] = std::move(replacement);
        dirty_shards.insert(global_id);
        report.add_index_changed(global_id);
        search_file_changed(global_id);
    }
    // A file's instantiations live with its shard of the same generation:
    // a rebuilt file's stored ones, read or not, are another generation's.
    for(auto path_id: rebuilt_ids) {
        retire_instantiations(path_id);
    }
    for(auto& [global_id, replacement]: instantiation_replacements) {
        instantiations[global_id] = std::move(replacement);
        dirty_instantiations.insert(global_id);
        retired_instantiations.erase(global_id);
        report.add_index_changed(global_id);
    }

    // Replace this TU's manifest wholesale: files it no longer touches lose
    // their contribution here, which is also what retires their variants —
    // no pass over other shards is needed.
    for(auto path_id: apply_manifest(tu_path_id, std::move(manifest))) {
        report.add_index_changed(path_id);
    }
    for(auto path_id: llvm::make_first_range(instantiation_replacements)) {
        refresh_live(path_id);
    }

    // A rebuild started its file's blob over, discarding the variants other
    // TUs' contributions pin. Those owners are usually already pending from
    // the same content event (enqueue dedupes); one whose attempt already
    // failed — or that reads fresh by hash after a revert, which is why
    // ContentChanged — has no in-process event left to rebuild its index,
    // only a read after a restart finding its variant lost.
    for(auto path_id: rebuilt_ids) {
        auto& shard = shards.find(path_id)->second;
        for_each_contribution(path_id, [&](Fid tu, std::uint64_t variant) {
            if(!shard.has_variant(variant)) {
                report.add_reindex(tu);
            }
        });
    }
    dirty_manifests.insert(tu_path_id);
    table_dirty = true;

    LOG_INFO(
        "Merged TUIndex: {} paths, {} sections ({} hits, {} appended, {} rebuilt), "
        "{} merged_shards",
        view.path_count(),
        view.section_count(),
        hits,
        appended,
        rebuilt_ids.size(),
        shards.size());

    return report;
}

std::vector<std::uint64_t> ProjectIndex::known_variants(Fid tu,
                                                        llvm::DenseSet<Fid> include_closure) const {
    auto& expected = include_closure;
    if(auto last = manifest(tu)) {
        for(auto version: llvm::make_first_range(last->contributions)) {
            expected.insert(files.version(version).fid);
        }
    }

    // What each blob stores: the held shard's variants, else the live
    // ones, which a blob is verified against when read — without reading
    // the blobs.
    std::vector<std::uint64_t> known;
    for(auto file: expected) {
        if(auto held = shards.find(file); held == shards.end()) {
            llvm::append_range(known, live_variants(file));
        } else if(held->second.loaded()) {
            llvm::append_range(known, held->second.variants());
        }
    }
    llvm::sort(known);
    known.erase(llvm::unique(known), known.end());
    return known;
}

std::vector<std::uint64_t> ProjectIndex::known_instantiations(Fid tu,
                                                              llvm::DenseSet<Fid> include_closure) {
    auto& expected = include_closure;
    if(auto last = manifest(tu)) {
        for(auto version: llvm::make_first_range(last->contributions)) {
            expected.insert(files.version(version).fid);
        }
    }
    expected.erase(tu);

    llvm::DenseMap<std::pair<std::uint64_t, std::uint64_t>, bool> holds;
    auto walk_holds = [&](const Producer& producer) {
        auto [it, inserted] = holds.try_emplace(std::pair(producer.unit, producer.run));
        if(inserted) {
            auto unit = find_unit(producer.unit);
            it->second = unit && *unit != tu && current_run(producer) && !needs_reindex(*unit);
        }
        return it->second;
    };
    std::vector<std::uint64_t> known;
    for(auto file: expected) {
        auto* stored = instantiation_shard(file);
        if(!stored) {
            continue;
        }
        stored->for_each_key([&](const InstantiationShard::Key& key) {
            if(walk_holds(*key.producer)) {
                known.push_back(key.key);
            }
            return true;
        });
    }
    llvm::sort(known);
    known.erase(llvm::unique(known), known.end());
    return known;
}

void ProjectIndex::retire_instantiations(Fid path_id) {
    instantiations.erase(path_id);
    dirty_instantiations.erase(path_id);
    retired_instantiations.insert(path_id);
}

void ProjectIndex::drop(Fid tu_path_id, IndexReport& report) {
    if(!has_manifest(tu_path_id)) {
        return;
    }
    // Dropped entries change index-served answers exactly like merged
    // entries do; without the refresh the client keeps them forever, since no
    // later merge or compile is owed.
    for(auto path_id: remove_manifest(tu_path_id)) {
        report.add_index_changed(path_id);
    }
    dirty_manifests.insert(tu_path_id);
    table_dirty = true;
}

void ProjectIndex::requeue_owners(Fid path_id, IndexReport& report) {
    for_each_contribution(path_id, [&](Fid tu, std::uint64_t) { report.add_reindex(tu); });
}

bool ProjectIndex::file_version_stale(VersionID version_id) {
    auto [cached, inserted] = version_stale.try_emplace(version_id, true);
    if(!inserted) {
        return cached->second;
    }

    // Missing and unreadable both read as stale — conservative, the
    // reindex re-observes.
    cached->second = files.check_version(version_id) != vfs::DiskState::CheckResult::Fresh;
    return cached->second;
}

const llvm::DenseSet<Fid>& ProjectIndex::stale_base_units() {
    if(stale_units) {
        return *stale_units;
    }
    auto& stale = stale_units.emplace();
    if(!base) {
        return stale;
    }
    // Each version is judged once, whatever number of units depend on it.
    auto& columns = *base->columns;
    for(std::uint32_t position = 0; position < columns.version_ids.size(); position += 1) {
        auto version = runtime_version(columns.version_ids[position]);
        if(version && !file_version_stale(*version)) {
            continue;
        }
        if(auto tus = base->version_units(position)) {
            for_each_base_unit(*tus, [&](Fid tu) { stale.insert(tu); });
        }
    }
    for(std::uint32_t k = 0; k < columns.absent_places.size(); k += 1) {
        if(!files.disk.present(base->remap[columns.absent_places[k]])) {
            continue;
        }
        if(auto tus = read_file_set(slice(columns.absent_units, columns.absent_unit_ends, k))) {
            for_each_base_unit(*tus, [&](Fid tu) { stale.insert(tu); });
        }
    }
    return stale;
}

bool ProjectIndex::needs_reindex(Fid file) {
    auto manifest_it = manifests.find(file);
    if(manifest_it == manifests.end()) {
        return !base_unit(file) || stale_base_units().contains(file);
    }

    // Every referenced version must be validated: whichever a partial
    // iteration skipped would keep serving a stale index behind a fresh
    // result.
    auto& manifest = manifest_it->second;
    if(file_version_stale(manifest.tu_version)) {
        return true;
    }
    for(auto& node: manifest.nodes) {
        if(file_version_stale(VersionID{node.file})) {
            return true;
        }
    }
    if(llvm::any_of(manifest.imports,
                    [&](VersionID version) { return file_version_stale(version); })) {
        return true;
    }
    return llvm::any_of(manifest.absent, [&](VersionID version) {
        return files.disk.present(files.version(version).fid);
    });
}

}  // namespace clice::index
