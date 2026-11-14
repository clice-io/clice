module clice;

import :index.shard.instantiations;
import :index.project_index;
import :index.project_index.records;
import :index.shard;
import :index.indexing.tu_index;
import :index.project_index.manifest;
import :support.logging;
import :support.text;
import :vfs.disk_state;
import :vfs.file_system;
import :vfs.file_table;

namespace clice::index {

std::expected<IndexReport, MergeError> ProjectIndex::merge(llvm::StringRef tu_index,
                                                           llvm::ArrayRef<Input> imports,
                                                           std::uint64_t command,
                                                           std::uint64_t peak) {
    // Zero-copy consumption: the result stays serialized; a new variant's
    // blob bytes are sliced out and installed or merged without decoding
    // the TU index, and only genuinely new symbol names are materialized.
    auto view = TUIndex::from_bytes(tu_index);
    if(!view.loaded()) {
        LOG_WARN("An index result failed its checks; ignored");
        return std::unexpected(MergeError::Invalid);
    }
    auto focus_local_id = view.focus_file();
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
    manifest.peak = peak;
    manifest.command = command;

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
                "The index of {} is not merged: the entries for {} consumed other content than the compiler",
                tu_path,
                files.resolve(file_ids_map[local_id]));
            return false;
        }
        consumed_hashes[local_id] = content_hash;
        return true;
    };
    // The hash of what the unit read of a path. The section's own record
    // wins: for a hashless path it is the only hash naming the bytes the
    // index describes. A file the unit reads through its precompiled header
    // or a module has no buffer in the parse to hash: the unit read the copy
    // their build read, whose hash the build's inputs record.
    llvm::DenseMap<Fid, std::uint64_t> precompiled_reads;
    for(auto& dep: imports) {
        if(dep.version.valid()) {
            precompiled_reads.try_emplace(dep.file, files.version(dep.version).content_hash);
        }
    }
    auto read_hash = [&](std::uint32_t local_id) -> std::uint64_t {
        if(consumed_hashes[local_id] != 0) {
            return consumed_hashes[local_id];
        }
        if(auto hash = view.content_hash(local_id)) {
            return hash;
        }
        return precompiled_reads.lookup(file_ids_map[local_id]);
    };
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto blob_hash = view.section_hash(section);
        auto global_id = file_ids_map[local_id];

        // Fast path: the blob already stores this variant. The identity
        // hashes the blob bytes, which embed the content generation, so
        // one membership test of the file's record is the whole check —
        // the shard is not read.
        if(auto* record = file_record(global_id);
           record && ranges::contains(record->stored, blob_hash)) {
            if(!record_consumed(local_id, record->generation)) {
                return std::unexpected(MergeError::Invalid);
            }
            section_contributions.emplace_back(local_id, blob_hash);
            hits += 1;
            continue;
        }
        auto* shard = this->shard(global_id);

        auto bytes = view.section_blob(section);
        if(bytes.empty()) {
            LOG_INFO("{} is indexed again: {} stores no variant of the hash it names",
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
            LOG_WARN("The index of {} is not merged: the section for {} failed verification",
                     tu_path,
                     files.resolve(global_id));
            return std::unexpected(MergeError::Invalid);
        }
        auto fresh = Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(!fresh.loaded()) {
            LOG_WARN("The index of {} is not merged: the entries for {} do not form a valid shard",
                     tu_path,
                     files.resolve(global_id));
            return std::unexpected(MergeError::Invalid);
        }
        if(!record_consumed(local_id, fresh.content_hash())) {
            return std::unexpected(MergeError::Invalid);
        }

        Shard replacement;
        if(shard && shard->content_hash() == fresh.content_hash()) {
            // Same generation, new variant: merge it in, keeping every
            // stored variant — dead ones stay masked until the next save
            // compacts them.
            auto merged = merge_shards(*shard, shard->variants(), llvm::ArrayRef(fresh));
            replacement =
                Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(merged)));
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
        auto generation = read_hash(local_id);
        const InstantiationShard* stored = instantiation_shard(global_id);
        if(stored && stored->content_hash() != generation) {
            stored = nullptr;
        }

        std::vector<ResolutionHash> used;
        for(auto key: view.skipped_instantiations(section)) {
            auto found = stored ? stored->find_key(key) : std::nullopt;
            if(!found) {
                LOG_INFO("{} is indexed again: {} stores no instantiation it skipped",
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
                LOG_WARN(
                    "The index of {} is not merged: instantiations for {} do not form a valid blob",
                    tu_path,
                    files.resolve(global_id));
                return std::unexpected(MergeError::Invalid);
            }
            if(!record_consumed(local_id, fresh->content_hash())) {
                return std::unexpected(MergeError::Invalid);
            }
            // The walk reads a file the precompiled header served from the
            // disk: text that moved on since that build read it is another
            // generation than the parse's.
            if(generation != 0 && fresh->content_hash() != generation) {
                LOG_INFO("{} is indexed again: {} changed while the unit read it",
                         tu_path,
                         files.resolve(global_id));
                return std::unexpected(MergeError::Outdated);
            }
            used.append_range(fresh->resolutions());
        }
        ranges::sort(used);
        used.erase(ranges::unique(used).begin(), used.end());
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
            llvm::MemoryBuffer::getMemBufferCopy(merged ? as_chars(*merged) : ""));
        assert(replacement.loaded() && "a freshly merged instantiation blob must verify");
        instantiation_replacements.emplace_back(global_id, std::move(replacement));
    }

    // The first commit: what the external symbols are, before the shards
    // below say which files hold their entries.
    llvm::SmallVector<SymbolHash> described;
    merge_facts(view, described);
    for(auto hash: described) {
        search.symbol_changed(hash);
    }

    // Intern a FileVersion per file of the parse: the consumed-content hash
    // from the compiler's own buffers, shared by every TU that consumed it
    // (see file_version_stale).
    auto baseline_before_ns = vfs::stat_baseline_before_ns(view.built_at());
    llvm::SmallVector<VersionID> version_of;
    version_of.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        llvm::StringRef path = view.path(i);
        auto hash = read_hash(i);

        // The worker had no buffer to hash a file behind a PCM, and no
        // shard or build recorded one.
        if(hash != 0) {
            files.disk.consumed(file_ids_map[i], hash);
        } else if(auto status = fs::status(path)) {
            hash = files.disk.unchanged_hash(file_ids_map[i], *status, baseline_before_ns);
        }

        version_of[i] = files.intern_version(file_ids_map[i], hash);
    }

    manifest.tu_version = version_of[focus_local_id];
    manifest.nodes.reserve(view.node_count());
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        auto node = view.node(i);
        node.file = version_of[node.file].raw;
        manifest.nodes.push_back(node);
    }
    for(std::uint32_t i = 0; i < view.link_count(); i += 1) {
        auto link = view.link(i);
        manifest.links.push_back({
            .file = version_of[link.file],
            .line = link.line,
            .column = link.column,
            .target = files.intern_version(files.intern(Spelling::absolute(link.target)), 0),
        });
    }
    for(auto [local_id, variant]: section_contributions) {
        manifest.contributions.emplace_back(version_of[local_id], variant);
    }
    for(auto [local_id, use]:
        views::transform(unit_uses, [](auto& entry) -> auto& { return entry.second; })) {
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
        if(dep.absent) {
            manifest.absent.push_back(files.intern_version(dep.file, 0));
        } else {
            manifest.imports.push_back(dep.version.valid() ? dep.version
                                                           : files.intern_version(dep.file, 0));
        }
    }

    for(auto& [global_id, replacement]: replacements) {
        replace_shard(global_id, std::move(replacement));
        report.add_index_changed(global_id);
    }
    // A file's instantiations live with its shard of the same generation.
    for(auto path_id: rebuilt_ids) {
        auto* held = instantiation_shard(path_id);
        if(held && held->content_hash() != shard(path_id)->content_hash()) {
            replace_instantiations(path_id, {});
        }
    }
    for(auto& [global_id, replacement]: instantiation_replacements) {
        replace_instantiations(global_id, std::move(replacement));
        report.add_index_changed(global_id);
    }
    list_locals(view, file_ids_map);
    // A symbol the unit describes that no stored entry mentions — the
    // template of an instantiation resolving nothing — keeps no record.
    for(auto hash: described) {
        fileless.emplace_back(hash, false);
    }
    drop_fileless_symbols();

    // Replace this TU's manifest wholesale: files it no longer touches lose
    // their contribution here, which is also what retires their variants —
    // no pass over other shards is needed.
    for(auto path_id: apply_manifest(tu_path_id, std::move(manifest))) {
        report.add_index_changed(path_id);
    }
    for(auto path_id: views::keys(instantiation_replacements)) {
        refresh_live(path_id);
    }

    // A rebuild started its file's blob over, discarding the variants other
    // TUs' contributions pin. Those owners are usually already pending from
    // the same content event (enqueue dedupes); one whose attempt already
    // failed — or that reads fresh by hash after a revert, which is why
    // ContentChanged — has no in-process event left to rebuild its index.
    for(auto path_id: rebuilt_ids) {
        auto* rebuilt = shard(path_id);
        for_each_contribution(path_id, [&](Fid tu, std::uint64_t variant) {
            if(!rebuilt->has_variant(variant)) {
                owe(tu, report);
            }
        });
    }
    // A run that started after the unit became outdated has seen what the
    // disk holds.
    if(auto it = outdated.find(tu_path_id); it != outdated.end() && it->second < view.built_at()) {
        outdated.erase(it);
    }

    LOG_DEBUG("Merged an index: {} paths, {} sections ({} hits, {} appended, {} rebuilt)",
              view.path_count(),
              view.section_count(),
              hits,
              appended,
              rebuilt_ids.size());

    return report;
}

std::vector<std::uint64_t> ProjectIndex::known_variants(Fid tu,
                                                        llvm::DenseSet<Fid> include_closure) const {
    auto& expected = include_closure;
    if(auto last = manifest(tu)) {
        for(auto version: views::keys(last->contributions)) {
            expected.insert(files.version(version).fid);
        }
    }

    std::vector<std::uint64_t> known;
    for(auto file: expected) {
        if(auto* record = file_record(file)) {
            for(auto variant: record->stored) {
                known.push_back(known_at(files.resolve(file), variant));
            }
        }
    }
    ranges::sort(known);
    return known;
}

std::vector<std::uint64_t>
    ProjectIndex::known_instantiations(Fid tu, llvm::DenseSet<Fid> include_closure) const {
    auto& expected = include_closure;
    if(auto last = manifest(tu)) {
        for(auto version: views::keys(last->contributions)) {
            expected.insert(files.version(version).fid);
        }
    }
    expected.erase(tu);

    llvm::DenseMap<std::pair<std::uint64_t, std::uint64_t>, bool> holds;
    auto walk_holds = [&](const Producer& producer) {
        auto [it, inserted] = holds.try_emplace(std::pair(producer.unit, producer.run));
        if(inserted) {
            auto unit = find_unit(producer.unit);
            it->second = unit && *unit != tu && current_run(producer) && !outdated.contains(*unit);
        }
        return it->second;
    };
    std::vector<std::uint64_t> known;
    for(auto file: expected) {
        auto* record = file_record(file);
        auto* held = record ? instantiation_shard(file) : nullptr;
        if(!held || record->generation != held->content_hash()) {
            continue;
        }
        held->for_each_key([&](const InstantiationShard::Key& key) {
            if(walk_holds(*key.producer)) {
                known.push_back(known_at(files.resolve(file), key.key));
            }
            return true;
        });
    }
    ranges::sort(known);
    return known;
}

void ProjectIndex::drop(Fid tu_path_id, IndexReport& report) {
    // Dropped entries change index-served answers exactly like merged
    // entries do; without the refresh the client keeps them forever, since no
    // later merge or compile is owed.
    for(auto path_id: remove_manifest(tu_path_id)) {
        report.add_index_changed(path_id);
    }
}

void ProjectIndex::mark_outdated(Fid tu) {
    outdated[tu] =
        chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch())
            .count();
}

void ProjectIndex::requeue_owners(Fid path_id, IndexReport& report) {
    for_each_contribution(path_id, [&](Fid tu, std::uint64_t) { owe(tu, report); });
}

void ProjectIndex::owe(Fid tu, IndexReport& report) {
    report.add_reindex(tu);
    header_changed = owed.insert(tu).second || header_changed;
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

bool ProjectIndex::needs_reindex(Fid file) {
    auto current = this->manifest(file);
    if(!current) {
        return true;
    }

    // Every referenced version must be validated: whichever a partial
    // iteration skipped would keep serving a stale index behind a fresh
    // result.
    auto& manifest = *current;
    if(file_version_stale(manifest.tu_version)) {
        return true;
    }
    for(auto& node: manifest.nodes) {
        if(file_version_stale(VersionID{node.file})) {
            return true;
        }
    }
    if(ranges::any_of(manifest.imports,
                      [&](VersionID version) { return file_version_stale(version); })) {
        return true;
    }
    return ranges::any_of(manifest.absent, [&](VersionID version) {
        return files.disk.present(files.version(version).fid);
    });
}

}  // namespace clice::index
