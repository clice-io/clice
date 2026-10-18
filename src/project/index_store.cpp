module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.format;
import :index.instantiation_shard;
import :index.query;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
import :project.command_resolver;
import :project.hosting;
import :project.index_store;
import :support.json;
import :support.logging;
import :support.timer;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// Entry hash of a file's default selection — the candidate-order winner,
/// or the default command covering a file without entries; empty when the
/// build does not compile it.
std::string selected_hash(Project& project, Fid file) {
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return {};
    }
    return project.cdb.entry_hash_hex(commands.front().config);
}

/// JSON layout of the persisted CDB snapshot (blob kind CDB): per source
/// file, the sorted canonical command hashes of its entries and a hash of
/// its matched config rules when the index state was last saved. A
/// standalone-indexed header gets an entry too (empty hashes): its own
/// matched rules plus the host source whose command its index borrowed.
/// Paths are the database's names (ProjectIndex::portable).
struct CDBSnapshotEntry {
    std::string file;
    std::vector<std::string> hashes;

    /// Entry hash of the file's default selection (the candidate-order
    /// winner, or the default command covering a file without entries).
    /// The hash multiset alone cannot see an offline flip of the winner —
    /// candidates unchanged, selection changed.
    std::string selected;

    /// The databases that listed the file, so a later session can tell a
    /// removal from a database that failed to load.
    std::vector<std::string> sources;

    std::string rules;
    std::string host;

    /// Entry hash of the host's default selection. A host that is not
    /// itself indexed (a default-command unit under `index = false`) has
    /// no entry of its own to record its command in.
    std::string host_selected;
};

struct CommandSnapshot {
    std::vector<CDBSnapshotEntry> entries;

    /// The files recorded as provisional members (Build::recorded), sorted.
    std::vector<std::string> provisional;
};

/// clice.toml rules change the effective indexing command without touching
/// the CDB entry, so the snapshot must cover their edits too — an offline
/// rule edit is as stale-making as an offline command edit. A file without
/// candidates that a rule's default command covers records that command's
/// identity as its selection, so an offline edit of the command text is
/// caught the same way.
CommandSnapshot build_cdb_snapshot(Project& project,
                               const llvm::DenseMap<Fid, Fid>& header_hosts,
                               llvm::ArrayRef<Fid> standalone_debt) {
    CommandSnapshot snapshot;
    for(auto& bucket: project.cdb.command_hash_snapshot()) {
        auto path_id = bucket.first;
        auto candidates = project.build.entries(path_id);
        if(candidates.empty()) {
            // Entries only inactive configurations declare: not compiled
            // by this view, so not part of its identity.
            continue;
        }
        auto file = project.file_table.resolve(path_id);
        auto rules = project.build.edit_hash(file);
        // In build order, which registration order — the order databases
        // were discovered in — must not leak into: the sequence is the
        // file's command identity across sessions.
        std::vector<std::string> hashes;
        std::vector<std::string> sources;
        for(auto& candidate: candidates) {
            hashes.push_back(project.cdb.entry_hash_hex(candidate.config));
            auto database = project.cdb.source_path(candidate.source);
            // Commands added by hand come from no database a later session
            // could miss.
            if(database.empty()) {
                continue;
            }
            auto source = project.index.portable(database);
            if(!llvm::is_contained(sources, source)) {
                sources.push_back(std::move(source));
            }
        }
        snapshot.entries.push_back({
            .file = project.index.portable(file),
            .hashes = std::move(hashes),
            .selected = project.cdb.entry_hash_hex(candidates.front().config),
            .sources = std::move(sources),
            .rules = std::move(rules),
        });
    }
    // Standalone-indexed TUs have no CDB entry, yet their effective command
    // depends on the default command covering them or the host they borrow
    // from, and on the rules matching them and that host — all of it must
    // be snapshot to detect offline changes.
    auto add_standalone = [&](Fid tu) {
        auto file = project.file_table.resolve(tu);
        if(!project.build.entries(tu).empty()) {
            return;
        }
        std::string host;
        std::string host_selected;
        llvm::SmallVector<CanonicalRef, 2> edit_paths;
        if(auto host_it = header_hosts.find(tu); host_it != header_hosts.end()) {
            auto host_path = project.file_table.resolve(host_it->second);
            host = project.index.portable(host_path);
            host_selected = selected_hash(project, host_it->second);
            edit_paths.push_back(host_path);
        }
        edit_paths.push_back(file);
        auto rules = project.build.edit_hash(edit_paths);
        snapshot.entries.push_back({
            .file = project.index.portable(file),
            .selected = selected_hash(project, tu),
            .rules = std::move(rules),
            .host = std::move(host),
            .host_selected = std::move(host_selected),
        });
    };
    for(auto tu: llvm::make_first_range(project.index.manifests)) {
        add_standalone(tu);
    }
    // A dropped standalone TU whose rebuild has not finished keeps its entry:
    // with no manifest pin and no CDB entry, the snapshot is the only
    // record that an index is owed (reconcile's debt pass retries it).
    for(auto tu: standalone_debt) {
        add_standalone(tu);
    }
    for(auto file: project.build.recorded()) {
        snapshot.provisional.push_back(
            project.index.portable(project.file_table.resolve(file)));
    }
    // Deterministic bytes: save() decides "unchanged" by byte equality.
    std::ranges::sort(snapshot.entries, {}, &CDBSnapshotEntry::file);
    std::ranges::sort(snapshot.provisional);
    return snapshot;
}

std::string serialize_cdb_snapshot(Project& project,
                                   const llvm::DenseMap<Fid, Fid>& header_hosts,
                                   llvm::ArrayRef<Fid> standalone_debt) {
    auto json = kota::codec::json::to_string<PathJsonConfig>(
        build_cdb_snapshot(project, header_hosts, standalone_debt));
    if(!json) {
        LOG_WARN("Failed to serialize the CDB snapshot: {}", json.error().to_string());
        return {};
    }
    return std::move(*json);
}

/// The artifacts blob: a JSON document in the index database, the
/// persisted form of PCH/PCM validity metadata and header-mode results
/// (the cache.json successor). Paths are
/// persisted as spellings and re-interned at load — runtime fids are not
/// stable across sessions; deps persist as (spelling, content hash), so
/// the records reference no version ids and self-heal against any index
/// state.

struct CacheDepEntry {
    std::uint32_t path;  // index into the blob's paths table
    std::uint64_t hash;
    bool missing;
};

struct CachePCHEntry {
    std::string key;
    /// CacheStore key in the "pch" namespace; absent in records written
    /// before pairs had names of their own.
    std::optional<std::string> blob;
    std::uint32_t bound;
    std::vector<CacheDepEntry> deps;
    /// See PCHState::base; absent for none.
    std::optional<std::string> base;
};

struct CachePCMEntry {
    std::string key;
    /// CacheStore key in the "pcm" namespace; absent in records written
    /// before PCMs had names of their own.
    std::optional<std::string> blob;
    std::uint32_t source_file;
    std::string module_name;
    std::vector<CacheDepEntry> deps;
};

struct ArtifactRecords {
    std::vector<std::string> paths;

    // index_format_version the .pch.idx indexes were written with (one
    // binary writes them all). A mismatch drops every PCH entry at load so
    // the pairs rebuild immediately, instead of the mismatch showing up
    // lazily on the first overlay query — which cannot trigger a rebuild.
    std::uint32_t pch_index_format = 0;

    std::vector<CachePCHEntry> pch;
    std::vector<CachePCMEntry> pcm;
    std::vector<CacheModeEntry> header_modes;
};

}  // namespace

IndexStore::IndexStore(kota::event_loop& loop, Project& project, CommandResolver& commands) :
    loop(loop), project(project), commands(commands) {}

std::string IndexStore::serialize_artifacts() {
    ArtifactRecords data;
    data.pch_index_format = index::index_format_version;
    llvm::StringMap<std::uint32_t> index_map;

    auto intern = [&](Fid fid) -> std::uint32_t {
        auto path = project.index.portable(project.file_table.resolve(fid));
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };

    // Deps persist as (spelling, hash) pairs so they self-heal against any
    // index state. A version-less dep (missing or unhashable at capture)
    // writes hash 0 and reloads as one.
    auto dump_deps = [&](const DepsSnapshot& snap, std::vector<CacheDepEntry>& out) {
        for(auto& dep: snap) {
            auto hash =
                dep.version.valid() ? project.file_table.version(dep.version).content_hash : 0;
            out.push_back({intern(dep.path_id), hash, dep.missing});
        }
    };

    for(auto& e: project.pch_cache) {
        auto& st = e.second;
        if(st.path.empty())
            continue;
        CachePCHEntry entry;
        entry.key = e.getKey().str();
        entry.blob = st.blob;
        entry.bound = st.bound;
        dump_deps(st.deps, entry.deps);
        if(!st.base.empty()) {
            entry.base = st.base;
        }
        data.pch.push_back(std::move(entry));
    }

    for(auto& [path_id, st]: project.pcm_builds) {
        if(st.path.empty())
            continue;
        CachePCMEntry entry;
        entry.key = st.key;
        entry.blob = st.blob;
        entry.source_file = intern(path_id);
        entry.module_name = project.dep_graph.module_of(path_id).str();
        dump_deps(st.deps, entry.deps);
        data.pcm.push_back(std::move(entry));
    }

    commands.dump_mode_slices(data.header_modes, intern);

    auto json = kota::codec::json::to_string<PathJsonConfig>(data);
    if(!json) {
        LOG_WARN("Failed to serialize the artifacts blob");
        return {};
    }
    return std::move(*json);
}

void IndexStore::load_artifacts(llvm::StringRef bytes) {
    if(bytes.empty() || !project.store) {
        return;
    }
    ArtifactRecords data;
    if(!kota::codec::json::from_string(bytes, data)) {
        LOG_WARN("Failed to parse the artifacts blob");
        return;
    }

    auto file_of = [&](std::uint32_t idx) -> std::optional<Fid> {
        if(idx >= data.paths.size() || data.paths[idx].empty()) {
            return std::nullopt;
        }
        return project.file_table.intern(project.index.local(data.paths[idx]));
    };
    auto load_deps = [&](const std::vector<CacheDepEntry>& dep_entries) -> DepsSnapshot {
        DepsSnapshot deps;
        for(auto& dep: dep_entries) {
            auto dep_file = file_of(dep.path);
            if(!dep_file)
                continue;
            auto& state = deps.emplace_back();
            state.path_id = *dep_file;
            state.missing = dep.missing;
            if(dep.hash != 0) {
                state.version = project.file_table.intern_version(state.path_id, dep.hash);
            }
        }
        return deps;
    };

    bool pch_format_ok = data.pch_index_format == index::index_format_version;
    for(auto& entry: data.pch) {
        if(!pch_format_ok) {
            break;
        }
        if(!entry.blob) {
            project.store->invalidate("pch", entry.key);
            continue;
        }
        auto pch_path = project.store->lookup("pch", *entry.blob);
        if(!pch_path)
            continue;
        // A PCH without its pch.idx index is an incomplete pair
        // (crash between the two commits): drop it so the next compile
        // rebuilds both.
        auto index_path = project.store->lookup_aux("pch", *entry.blob);
        if(!index_path) {
            project.store->invalidate("pch", *entry.blob);
            continue;
        }

        auto& st = project.pch_cache[entry.key];
        st.blob = std::move(*entry.blob);
        st.path = *pch_path;
        st.bound = entry.bound;
        st.deps = load_deps(entry.deps);
        st.base = entry.base.value_or("");
        st.index_path = *index_path;
    }

    for(auto& entry: data.pcm) {
        if(!entry.blob) {
            project.store->invalidate("pcm", entry.key);
            continue;
        }
        auto pcm_path = project.store->lookup("pcm", *entry.blob);
        // A dep-less entry is an unvalidatable snapshot that would blindly
        // serve a stale PCM (the key embeds no content). Drop it and let
        // the module rebuild once.
        if(!pcm_path || entry.deps.empty())
            continue;
        auto source = file_of(entry.source_file);
        if(!source)
            continue;
        auto& st = project.pcm_builds[*source];
        st.path = *pcm_path;
        st.key = entry.key;
        st.blob = std::move(*entry.blob);
        st.deps = load_deps(entry.deps);
    }

    commands.load_mode_slices(data.header_modes, file_of);

    LOG_INFO("Loaded artifact metadata: {} PCH entries, {} PCM entries",
             project.pch_cache.size(),
             project.pcm_builds.size());
}

std::expected<IndexStore::Report, IndexStore::MergeError>
    IndexStore::merge(const void* tu_index_data,
                      std::size_t size,
                      llvm::ArrayRef<DepState> imports) {
    // Zero-copy consumption: the result stays serialized; a new variant's
    // blob bytes are sliced out and installed or merged without decoding
    // the TU index, and only genuinely new symbol names are materialized.
    auto view =
        index::TUIndex::from_bytes(llvm::StringRef(static_cast<const char*>(tu_index_data), size));
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
    auto& project_index = project.index;
    llvm::SmallVector<Fid> file_ids_map;
    file_ids_map.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map[i] = project.file_table.intern(Spelling::absolute(view.path(i)));
    }
    auto tu_path_id = file_ids_map[focus_local_id];

    index::TUManifest manifest;
    manifest.built_at = static_cast<std::uint64_t>(view.built_at());

    std::size_t hits = 0;
    std::size_t appended = 0;
    // Staged, not committed: a section that fails verification rejects the
    // whole result mid-loop, and shards installed before that point would
    // leave the surviving manifest referencing variants the new blobs no
    // longer store.
    llvm::SmallVector<std::pair<Fid, index::Shard>> replacements;
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
            LOG_WARN("Reject merge for {}: rows for {} consumed other content than the compiler",
                     tu_path,
                     project.file_table.resolve(file_ids_map[local_id]));
            return false;
        }
        consumed_hashes[local_id] = content_hash;
        return true;
    };
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto blob_hash = view.section_hash(section);
        auto global_id = file_ids_map[local_id];

        auto shard_it = project.index.shards.find(global_id);
        auto* shard = shard_it != project.index.shards.end() ? &shard_it->second : nullptr;

        // Fast path: the blob already stores this variant. The identity
        // hashes the blob bytes, which embed the content generation, so
        // one membership test is the whole check — no IO, no bytes read.
        if(shard && shard->loaded() && shard->has_variant(blob_hash)) {
            if(!record_consumed(local_id, shard->content_hash())) {
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
                     project.file_table.resolve(global_id));
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
            LOG_WARN("Reject merge for {}: rows section for {} failed verification",
                     tu_path,
                     project.file_table.resolve(global_id));
            return std::unexpected(MergeError::Invalid);
        }
        auto fresh = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(!fresh.loaded()) {
            LOG_WARN("Reject merge for {}: rows for {} do not form a valid shard",
                     tu_path,
                     project.file_table.resolve(global_id));
            return std::unexpected(MergeError::Invalid);
        }
        if(!record_consumed(local_id, fresh.content_hash())) {
            return std::unexpected(MergeError::Invalid);
        }

        index::Shard replacement;
        if(shard && shard->loaded() && shard->content_hash() == fresh.content_hash()) {
            // Same generation, new variant: merge it in, keeping every
            // stored variant — dead ones stay masked until the next save
            // compacts them.
            std::string merged;
            llvm::raw_string_ostream os(merged);
            index::merge_shards(*shard, shard->variants(), llvm::ArrayRef(fresh), os);
            replacement = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(merged));
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
    index::Producer producer{
        .unit = project_index.unit_hash(project.file_table, tu_path_id),
        .run = static_cast<std::uint64_t>(view.built_at()),
    };
    llvm::SmallVector<std::pair<Fid, index::InstantiationShard>> instantiation_replacements;
    llvm::DenseMap<Fid, std::pair<std::uint32_t, index::UseHash>> unit_uses;
    for(std::uint32_t section = 0; section < view.instantiation_count(); section += 1) {
        auto local_id = view.instantiation_path(section);
        auto global_id = file_ids_map[local_id];
        auto generation =
            consumed_hashes[local_id] != 0 ? consumed_hashes[local_id] : view.content_hash(local_id);
        auto stored_it = project_index.instantiations.find(global_id);
        const index::InstantiationShard* stored =
            stored_it != project_index.instantiations.end() &&
                    stored_it->second.content_hash() == generation
                ? &stored_it->second
                : nullptr;

        std::vector<index::ResolutionHash> used;
        for(auto key: view.skipped_instantiations(section)) {
            auto found = stored ? stored->find_key(key) : std::nullopt;
            if(!found) {
                LOG_INFO("Rerun {}: {} stores no instantiation it skipped",
                         tu_path,
                         project.file_table.resolve(global_id));
                return std::unexpected(MergeError::Outdated);
            }
            if(found->resolution) {
                used.push_back(*found->resolution);
            }
        }

        auto bytes = view.instantiation_blob(section);
        std::optional<index::InstantiationShard> fresh;
        if(!bytes.empty()) {
            fresh = index::InstantiationShard::from_bytes(bytes);
            if(!fresh->loaded()) {
                LOG_WARN("Reject merge for {}: instantiations for {} do not form a valid blob",
                         tu_path,
                         project.file_table.resolve(global_id));
                return std::unexpected(MergeError::Invalid);
            }
            if(!record_consumed(local_id, fresh->content_hash())) {
                return std::unexpected(MergeError::Invalid);
            }
            llvm::append_range(used, fresh->resolutions());
        }
        llvm::sort(used);
        used.erase(llvm::unique(used), used.end());
        std::optional<index::AddedUse> added;
        if(!used.empty()) {
            added = index::AddedUse{.use = index::use_hash(used), .resolutions = used};
            unit_uses[global_id] = {local_id, added->use};
        }
        // Walking nothing new, a unit naming a use the file stores already
        // changes nothing there.
        if(!fresh && (!added || stored->has_use(added->use))) {
            continue;
        }
        const static index::InstantiationShard none;
        const auto& old = stored ? *stored : none;
        std::string merged;
        llvm::raw_string_ostream os(merged);
        index::merge_instantiations(
            old,
            old.uses(),
            [&](const index::Producer& run) { return project_index.current_run(run); },
            fresh ? &*fresh : nullptr,
            producer,
            added,
            os);
        auto replacement =
            index::InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(merged));
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
    llvm::SmallVector<index::SymbolHash> added;
    project_index.merge_symbols(view, file_ids_map, &added);
    project.index.search_pending.insert(added.begin(), added.end());
    if(changed_during_search_build) {
        changed_during_search_build->symbols.insert(added.begin(), added.end());
    }
    merges_since_search_build += 1;

    // Intern a FileVersion per file of the parse: the consumed-content hash
    // from the compiler's own buffers, shared by every TU that consumed it
    // (see file_version_stale).
    auto baseline_before_ns = vfs::stat_baseline_before_ns(view.built_at());
    llvm::SmallVector<VersionID> fv_of;
    fv_of.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        llvm::StringRef path = view.path(i);
        // The section's own record wins: for a hashless path (behind a
        // PCM) it is the only hash naming the bytes the index describes.
        auto hash = consumed_hashes[i] != 0 ? consumed_hashes[i] : view.content_hash(i);

        if(hash != 0) {
            project.file_table.disk.consumed(file_ids_map[i], hash);
        } else {
            auto status = vfs::status(path);
            if(status && status->stamp.mtime_ns <= baseline_before_ns) {
                // The worker had no buffer to hash (e.g. behind a PCM) and
                // no shard recorded one; the unchanged mtime proves the disk
                // still holds the consumed bytes, so take their hash from
                // the last reliable read — or one read, unless the file
                // moved between the stat and the read, which voids the
                // proof.
                auto obs = project.file_table.disk.observe_for(file_ids_map[i], *status);
                if(obs && obs->stamp == status->stamp) {
                    hash = obs->hash;
                }
            }
        }

        fv_of[i] = project.file_table.intern_version(file_ids_map[i], hash);
    }

    manifest.tu_fv = fv_of[focus_local_id];
    manifest.nodes.reserve(view.node_count());
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        auto node = view.node(i);
        manifest.nodes.push_back({.file = fv_of[node.file].raw,
                                  .parent = node.parent,
                                  .line = node.line,
                                  .skipped = node.skipped});
    }
    for(auto [local_id, variant]: section_contributions) {
        manifest.contributions.emplace_back(fv_of[local_id], variant);
    }
    for(auto& local: *shared_locals) {
        index::SharedLocal entry{.symbol = local.symbol};
        for(auto path_id: local.paths) {
            entry.files.push_back(fv_of[path_id]);
        }
        manifest.shared_locals.push_back(std::move(entry));
    }
    for(auto [local_id, use]: llvm::make_second_range(unit_uses)) {
        manifest.instantiations.push_back({.file = fv_of[local_id], .use = use});
    }

    // The places the parse's failed lookups looked: the file table watches
    // them from here on. One that holds a file by now makes the index stale
    // on arrival.
    Report report;
    for(std::uint32_t i = 0; i < view.absent_count(); i += 1) {
        auto fid = project.file_table.intern(Spelling::absolute(view.absent(i)));
        if(project.file_table.disk.current(fid)) {
            report.add_reindex(tu_path_id);
        }
        manifest.absent.push_back(project.file_table.intern_version(fid, 0));
    }

    // The inputs of the imported modules and of the precompiled header
    // never enter the parse's include tree, yet each is an input of the
    // parse all the same. Not stale on arrival like the parse's own
    // lookups: a reindex would read the same PCM, which only a change
    // event reaching the module rebuilds. An input the module's build
    // could not version reads as stale until a rebuild names its bytes.
    for(auto& dep: imports) {
        if(dep.missing) {
            manifest.absent.push_back(project.file_table.intern_version(dep.path_id, 0));
        } else {
            manifest.imports.push_back(dep.version.valid()
                                           ? dep.version
                                           : project.file_table.intern_version(dep.path_id, 0));
        }
    }

    for(auto& [global_id, replacement]: replacements) {
        project.index.shards[global_id] = std::move(replacement);
        dirty_shards.insert(global_id);
        report.add_index_changed(global_id);
        search_file_changed(global_id);
    }
    // A file's instantiations live with its shard of the same generation.
    for(auto path_id: rebuilt_ids) {
        auto it = project_index.instantiations.find(path_id);
        if(it != project_index.instantiations.end() &&
           it->second.content_hash() != project_index.shards.find(path_id)->second.content_hash()) {
            retire_instantiations(path_id);
        }
    }
    for(auto& [global_id, replacement]: instantiation_replacements) {
        project_index.instantiations[global_id] = std::move(replacement);
        dirty_instantiations.insert(global_id);
        retired_instantiations.erase(global_id);
        report.add_index_changed(global_id);
    }

    // Replace this TU's manifest wholesale: files it no longer touches lose
    // their contribution here, which is also what retires their variants —
    // no pass over other shards is needed.
    auto affected =
        project_index.apply_manifest(project.file_table, tu_path_id, std::move(manifest));
    for(auto path_id: affected) {
        report.add_index_changed(path_id);
        refresh_live(path_id);
    }
    for(auto path_id: llvm::make_first_range(instantiation_replacements)) {
        refresh_live(path_id);
    }

    // A rebuild started its file's blob over, discarding the variants other
    // TUs' contributions pin. Those owners are usually already pending from
    // the same content event (enqueue dedupes); one whose attempt already
    // failed — or that reads fresh by hash after a revert, which is why
    // ContentChanged — has no in-process event left to rebuild its index,
    // only a restart reaching load()'s re-enqueue.
    for(auto path_id: rebuilt_ids) {
        auto& shard = project.index.shards.find(path_id)->second;
        for(auto& [tu, hash]: project_index.contributions.find(path_id)->second) {
            if(!shard.has_variant(hash)) {
                report.add_reindex(tu);
            }
        }
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
        project.index.shards.size());

    return report;
}

std::vector<std::uint64_t> IndexStore::known_variants(Fid tu) const {
    auto files = project.dep_graph.include_closure(tu);
    auto& index = project.index;
    if(auto it = index.manifests.find(tu); it != index.manifests.end()) {
        for(auto version: llvm::make_first_range(it->second.contributions)) {
            files.insert(project.file_table.version(version).fid);
        }
    }

    std::vector<std::uint64_t> known;
    for(auto file: files) {
        if(auto it = index.shards.find(file); it != index.shards.end() && it->second.loaded()) {
            llvm::append_range(known, it->second.variants());
        }
    }
    llvm::sort(known);
    known.erase(llvm::unique(known), known.end());
    return known;
}

std::vector<std::uint64_t> IndexStore::known_instantiations(Fid tu) {
    auto files = project.dep_graph.include_closure(tu);
    auto& index = project.index;
    if(auto it = index.manifests.find(tu); it != index.manifests.end()) {
        for(auto version: llvm::make_first_range(it->second.contributions)) {
            files.insert(project.file_table.version(version).fid);
        }
    }
    files.erase(tu);

    llvm::DenseMap<std::pair<std::uint64_t, std::uint64_t>, bool> holds;
    auto walk_holds = [&](const index::Producer& producer) {
        auto [it, inserted] = holds.try_emplace(std::pair(producer.unit, producer.run));
        if(inserted) {
            auto unit = index.find_unit(producer.unit);
            it->second = unit && *unit != tu && index.current_run(producer) && !needs_reindex(*unit);
        }
        return it->second;
    };
    std::vector<std::uint64_t> known;
    for(auto file: files) {
        auto it = index.instantiations.find(file);
        auto shard = index.shards.find(file);
        if(it == index.instantiations.end() || shard == index.shards.end() ||
           shard->second.content_hash() != it->second.content_hash()) {
            continue;
        }
        it->second.for_each_key([&](const index::InstantiationShard::Key& key) {
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

void IndexStore::retire_instantiations(Fid path_id) {
    project.index.instantiations.erase(path_id);
    dirty_instantiations.erase(path_id);
    retired_instantiations.insert(path_id);
}

IndexStore::Report IndexStore::drop_index(Fid tu_path_id) {
    Report report;
    drop_index_into(tu_path_id, report);
    return report;
}

void IndexStore::search_file_changed(Fid path_id) {
    project.index.search_pending_files.insert(path_id);
    if(changed_during_search_build) {
        changed_during_search_build->files.insert(path_id);
    }
}

void IndexStore::refresh_live(Fid path_id) {
    auto& index = project.index;
    if(auto it = index.instantiations.find(path_id); it != index.instantiations.end()) {
        it->second.set_live(index.live_uses(path_id));
    }
    auto it = index.shards.find(path_id);
    if(it == index.shards.end()) {
        return;
    }
    it->second.set_live(index.live_variants(path_id));
    search_file_changed(path_id);
}

void IndexStore::drop_index_into(Fid tu_path_id, Report& report) {
    auto& project_index = project.index;
    if(!project_index.manifests.contains(tu_path_id)) {
        return;
    }
    // Dropped entries change index-served answers exactly like merged
    // entries do; without the refresh the client keeps them forever, since no
    // later merge or compile is owed.
    for(auto path_id: project_index.remove_manifest(project.file_table, tu_path_id)) {
        refresh_live(path_id);
        report.add_index_changed(path_id);
    }
    dirty_manifests.insert(tu_path_id);
    table_dirty = true;
}

kota::task<IndexStore::Report> IndexStore::save(llvm::SmallVector<Fid> debt, SearchRebuild search) {
    Report report;
    // Reset up front: every early return below means this save committed
    // nothing, and the gauge must not keep exposing the previous round's
    // count as current.
    saved_shards = 0;
    // A read-only session (batch lint, stats) keeps its metadata in memory
    // and exits with it — it must never write into a database a concurrent
    // writer owns.
    if(!project.database || project.database->read_only())
        co_return report;
    co_await save_gate.acquire();
    auto gate = llvm::make_scope_exit([this] { save_gate.release(); });
    // Re-checked: the gate holder we just waited out may have hit
    // corruption and failed to reopen the database.
    if(!project.database || project.database->read_only())
        co_return report;
    auto& db = *project.database;
    auto& project_index = project.index;
    ScopedTimer timer;

    if(search_rebuild_due(search)) {
        co_await rebuild_search_index();
    }

    // Compact shards whose variant set shrank: queries already mask the
    // dead variants, this erases them for real before the blob reaches disk.
    // A file with no live variant left in its blob is retired entirely
    // below: either no contribution remains, or the remaining ones pin
    // hashes a newer content generation replaced (their TUs have not
    // reindexed yet) — compacting to those pins would write a blob with no
    // variants at all, while retiring serves the same nothing the mask
    // already does. The pinning TUs are re-enqueued: no in-process event
    // would rebuild their variants otherwise (a reverted file even reads fresh
    // by hash), only a restart reaching load()'s re-enqueue.
    llvm::SmallVector<Fid> retired;
    for(auto& [path_id, shard]: project.index.shards) {
        auto live = project_index.live_variants(path_id);
        if(llvm::none_of(live, [&](std::uint64_t hash) { return shard.has_variant(hash); })) {
            retired.push_back(path_id);
            continue;
        }
        if(!shard.has_dead_variants()) {
            continue;
        }
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::merge_shards(shard, live, {}, os);
        auto replacement = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        assert(replacement.loaded() && "a freshly written shard blob must verify");
        shard = std::move(replacement);
        dirty_shards.insert(path_id);
    }
    for(auto path_id: retired) {
        project.index.shards.erase(path_id);
        dirty_shards.erase(path_id);
        report.add_index_changed(path_id);
        requeue_owners(path_id, report);
    }

    // A file's instantiations live with its shard of the same generation.
    // Dead uses and the resolutions only they held compact like dead
    // variants, and the keys of runs that are no longer their unit's current
    // one leave with them; a file left with neither is retired.
    auto current_run = [&](const index::Producer& run) {
        return project_index.current_run(run);
    };
    llvm::SmallVector<Fid> retired_files;
    for(auto& [path_id, instantiations]: project_index.instantiations) {
        auto shard = project_index.shards.find(path_id);
        if(shard == project_index.shards.end() || !shard->second.loaded() ||
           shard->second.content_hash() != instantiations.content_hash()) {
            retired_files.push_back(path_id);
            continue;
        }
        bool gone_runs = false;
        instantiations.for_each_key([&](const index::InstantiationShard::Key& key) {
            gone_runs = !current_run(*key.producer);
            return !gone_runs;
        });
        if(!instantiations.has_dead_uses() && !gone_runs) {
            continue;
        }
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        if(!index::merge_instantiations(instantiations,
                                        project_index.live_uses(path_id),
                                        current_run,
                                        nullptr,
                                        {},
                                        std::nullopt,
                                        os)) {
            retired_files.push_back(path_id);
            continue;
        }
        auto replacement =
            index::InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        assert(replacement.loaded() && "a freshly written instantiation blob must verify");
        instantiations = std::move(replacement);
        dirty_instantiations.insert(path_id);
    }
    for(auto path_id: retired_files) {
        retire_instantiations(path_id);
        report.add_index_changed(path_id);
    }

    // Snapshot the dirty state on the loop: everything below serializes
    // from copies, so merges committing across the write await simply re-dirty
    // for the next save. The id vectors parallel the batch so a failed
    // entry can be re-dirtied by its batch index.
    std::vector<index::BlobDatabase::Blob> batch;
    llvm::SmallVector<Fid> shard_ids;
    llvm::SmallVector<Fid> manifest_ids;
    llvm::SmallVector<index::BlobKey> removals = std::move(startup_removes);
    startup_removes.clear();
    for(auto path_id: retired) {
        removals.push_back({index::BlobKind::Shard,
                            project.index.key_of(project.file_table, path_id)});
    }
    llvm::SmallVector<Fid> retired_ids(retired_instantiations.begin(),
                                       retired_instantiations.end());
    retired_instantiations.clear();
    for(auto path_id: retired_ids) {
        removals.push_back({index::BlobKind::InstantiationShard,
                            project.index.key_of(project.file_table, path_id)});
    }
    for(auto path_id: dirty_shards) {
        auto it = project.index.shards.find(path_id);
        assert(it != project.index.shards.end() &&
               "dirty shards stay resident until retirement");
        batch.push_back({index::BlobKind::Shard,
                         project.index.key_of(project.file_table, path_id),
                         it->second.bytes().str()});
        shard_ids.push_back(path_id);
    }
    auto shard_count = batch.size();
    llvm::SmallVector<Fid> instantiation_ids;
    for(auto path_id: dirty_instantiations) {
        auto it = project_index.instantiations.find(path_id);
        assert(it != project_index.instantiations.end() &&
               "dirty instantiation shards stay resident until retirement");
        batch.push_back({index::BlobKind::InstantiationShard,
                         project.index.key_of(project.file_table, path_id),
                         it->second.bytes().str()});
        instantiation_ids.push_back(path_id);
    }
    auto instantiation_count = batch.size() - shard_count;

    // One generation per persisted project table, recorded in every
    // manifest of the batch and pinned per TU inside the project table
    // (serialize_table): load() adopts a manifest only at its pinned
    // generation. Without the pin an ordering check alone cannot tell a
    // deliberately unchanged older manifest from one whose update failed
    // while the project table committed — both FileVersion sets can stay fully
    // resolvable (a reindex that changed entries or the include tree only).
    if(table_dirty) {
        project_index.table_generation += 1;
    }
    for(auto tu_path_id: dirty_manifests) {
        auto it = project_index.manifests.find(tu_path_id);
        auto key = project.index.key_of(project.file_table, tu_path_id);
        // Dirty with no in-memory manifest means dropped (drop_index): the
        // persisted blob must go too, or a restart resurrects the TU's
        // index as fresh.
        if(it == project_index.manifests.end()) {
            removals.push_back({index::BlobKind::TUManifest, std::move(key)});
            continue;
        }
        it->second.table_generation = project_index.table_generation;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(project_index.export_manifest(it->second), os);
        batch.push_back({index::BlobKind::TUManifest, std::move(key), std::move(bytes)});
        manifest_ids.push_back(tu_path_id);
    }
    auto manifest_count = batch.size() - shard_count - instantiation_count;

    // The project table follows the shards and manifests: a crash mid-batch
    // then strands only manifests, which load() drops by their generation
    // pin — the reverse order would strand a project table reporting symbols in
    // files whose shards never committed.
    std::optional<std::size_t> table_slot;
    if(table_dirty) {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        project_index.serialize_table(os, project.file_table);
        table_slot = batch.size();
        batch.push_back({index::BlobKind::ProjectTable, "global", std::move(bytes)});
    }
    // The search blob moves into the batch and back out if the write
    // never commits: it is derived state, so nothing else would re-create
    // it before the next rebuild.
    std::optional<std::size_t> search_slot;
    if(!search_bytes.empty()) {
        search_slot = batch.size();
        batch.push_back({index::BlobKind::SearchIndex, "search", std::move(search_bytes)});
        search_bytes.clear();
    }

    // The CDB snapshot goes last, after the whole index state it
    // describes: committed before the project table, a crash between the two would
    // leave the old project table still pinning old-command manifests under a
    // snapshot that already matches the live CDB — reconcile would then
    // never drop them. A save that commits nothing skips the recompute
    // (unless the snapshot itself is owed a rewrite): a pure CDB change
    // dirties no blob on its own — the invalidator's drops do — so the
    // next dirtying save carries the fresh snapshot.
    // The debt input is the pump's snapshot at call time plus the owner
    // debt the compaction above discovered; anything showing up past this
    // point is too late for these bytes and returns in the report.
    std::string cdb_bytes;
    std::optional<std::size_t> cdb_index;
    auto records = project.build.recorded_generation();
    if(!batch.empty() || !removals.empty() || cdb_dirty || records != serialized_records) {
        debt.append(report.reindex().begin(), report.reindex().end());
        cdb_bytes = serialize_cdb_snapshot(project, header_hosts, standalone_of(debt));
        serialized_records = records;
        if(!cdb_bytes.empty() && cdb_bytes != persisted_cdb_snapshot) {
            cdb_index = batch.size();
            batch.push_back({index::BlobKind::CommandSnapshot, "cdb", cdb_bytes});
        } else {
            cdb_dirty = false;
        }
    }

    // Metadata blobs ride every save: dirty artifact records and context
    // choices flush on whatever save runs next. The contexts ticket is
    // snapshotted here — marks arriving past this point are not covered by
    // these bytes and wait for the next save. The contexts blob's fate
    // is tracked on its own: a failing artifacts blob must not park a
    // switchContext ack whose choice is already durable.
    auto flush_ticket = contexts.ticket;
    std::optional<std::size_t> artifacts_index;
    std::optional<std::size_t> contexts_index;
    bool contexts_ok = true;
    if(project.artifacts_dirty) {
        if(auto bytes = serialize_artifacts(); !bytes.empty()) {
            artifacts_index = batch.size();
            batch.push_back({index::BlobKind::ArtifactRecords, "artifacts", std::move(bytes)});
        }
    }
    if(contexts.dirty) {
        if(!contexts.bytes.empty()) {
            contexts_index = batch.size();
            batch.push_back({index::BlobKind::ContextChoices, "contexts", contexts.bytes});
        } else {
            contexts_ok = false;
        }
    }

    bool had_table = table_dirty;
    dirty_shards.clear();
    dirty_instantiations.clear();
    dirty_manifests.clear();
    table_dirty = false;
    // Serialization failures keep their dirty flag for the next attempt.
    if(artifacts_index) {
        project.artifacts_dirty = false;
    }
    if(contexts_index) {
        contexts.dirty = false;
    }

    // A deferred load-time removal can name a key this very save re-writes:
    // the removed TU was re-enqueued at load and has already re-indexed.
    // Puts run before removes inside write(), so the stale removal would
    // delete the fresh blob — the put wins.
    if(!removals.empty()) {
        llvm::DenseSet<std::pair<unsigned, llvm::StringRef>> putting;
        for(auto& put: batch) {
            putting.insert({static_cast<unsigned>(put.kind), llvm::StringRef(put.key)});
        }
        llvm::erase_if(removals, [&](const index::BlobKey& remove) {
            return putting.contains(
                {static_cast<unsigned>(remove.kind), llvm::StringRef(remove.key)});
        });
    }

    if(batch.empty() && removals.empty()) {
        // A serialize-only failure builds no batch: waiters still must see
        // the failed attempt (see the failure pulse below), or a request
        // bounded on failed saves never counts one.
        if(!contexts_ok) {
            contexts.committed.set();
            contexts.committed.reset();
        }
        co_return report;
    }

    // The dirty set was snapshot-cleared above so merges committing across
    // the write await re-dirty for the next save; the in-flight count
    // keeps pending_shard_writes() truthful meanwhile — a stats reader
    // polling for "shard writes finished" must not observe zero while the
    // commit is still running (and saved_shards still holds its reset).
    saving_shards = shard_count + instantiation_count;
    llvm::SmallVector<std::size_t> failed;
    // Cancellation while the write is still queued dequeues it (see
    // kota::queue): the batch dies with this frame, so the snapshot-
    // cleared dirty state must be restored or the final shutdown save
    // sees nothing to commit and reports saved progress that is absent
    // after restart. Restoration unions with whatever re-dirtied across
    // the await; the startup removal keys in `removals` go back to the deferred
    // list they came from.
    // A merge across the await may retire a file's instantiations: retired,
    // they stay so.
    auto redirty_instantiations = [&](Fid path_id) {
        if(project_index.instantiations.contains(path_id)) {
            dirty_instantiations.insert(path_id);
        }
    };
    auto restore = llvm::make_scope_exit([&] {
        dirty_shards.insert(shard_ids.begin(), shard_ids.end());
        llvm::for_each(instantiation_ids, redirty_instantiations);
        for(auto path_id: retired_ids) {
            if(!project_index.instantiations.contains(path_id)) {
                retired_instantiations.insert(path_id);
            }
        }
        dirty_manifests.insert(manifest_ids.begin(), manifest_ids.end());
        table_dirty = table_dirty || had_table;
        project_index.restore_unwritten();
        if(search_slot) {
            search_bytes = std::move(batch[*search_slot].bytes);
        }
        cdb_dirty = cdb_dirty || cdb_index.has_value();
        project.artifacts_dirty = project.artifacts_dirty || artifacts_index.has_value();
        contexts.dirty = contexts.dirty || contexts_index.has_value();
        startup_removes.append(std::make_move_iterator(removals.begin()),
                               std::make_move_iterator(removals.end()));
        saving_shards = 0;
    });
    co_await kota::queue([&] { failed = db.write(batch, removals); });
    restore.release();
    // An entry the storage failed to commit is re-dirtied so a later save
    // retries it; discarded, the cache would trail the in-memory index
    // until an unrelated merge happens to dirty the same entry or a
    // restart rebuilds it. (Failed removals need no retry: load() drops
    // stale manifests by their generation pin and removes orphan shards.)
    std::size_t failed_shards = 0;
    for(auto i: failed) {
        if(i < shard_count) {
            failed_shards += 1;
            dirty_shards.insert(shard_ids[i]);
        } else if(i - shard_count < instantiation_count) {
            failed_shards += 1;
            redirty_instantiations(instantiation_ids[i - shard_count]);
        } else if(i - shard_count - instantiation_count < manifest_count) {
            dirty_manifests.insert(manifest_ids[i - shard_count - instantiation_count]);
        } else if(cdb_index && i == *cdb_index) {
            // Keep the old persisted bytes and stay dirty so the next
            // save retries even when nothing else changes by then.
            cdb_dirty = true;
            cdb_index.reset();
        } else if(artifacts_index && i == *artifacts_index) {
            project.artifacts_dirty = true;
        } else if(contexts_index && i == *contexts_index) {
            contexts.dirty = true;
            contexts_ok = false;
        } else if(search_slot && i == *search_slot) {
            search_bytes = std::move(batch[i].bytes);
        } else {
            table_dirty = true;
            table_slot.reset();
        }
    }
    // The entries the committed blob holds now read from it; a failed project table
    // write keeps them aside for the retry.
    if(table_slot) {
        project_index.rebase(llvm::MemoryBuffer::getMemBufferCopy(batch[*table_slot].bytes),
                             project.file_table);
    } else if(had_table) {
        project_index.restore_unwritten();
    }
    if(cdb_index) {
        persisted_cdb_snapshot = std::move(cdb_bytes);
        cdb_dirty = false;
    }
    saved_shards = shard_count + instantiation_count - failed_shards;
    saving_shards = 0;

    // Context choices as of the snapshot point are durable: advance the
    // committed ticket and wake durability waiters (switchContext). Failed
    // attempts pulse too, or a permanently failing write (full disk,
    // unserializable path) parks every durability wait forever; they give
    // up after a few of these.
    if(contexts_ok) {
        contexts.committed_ticket = std::max(contexts.committed_ticket, flush_ticket);
    }
    contexts.committed.set();
    contexts.committed.reset();

    // Corruption can show up first at write time (a damaged page only the
    // write's tree descent reaches): heal like load-time corruption instead
    // of writing into the damaged environment every save. The batch's
    // shards — dirty at batch build, hence memory-backed — re-dirty before
    // the recovery's shed so they survive it and re-persist wholesale.
    if(db.corrupted()) {
        for(auto path_id: shard_ids) {
            dirty_shards.insert(path_id);
        }
        llvm::for_each(instantiation_ids, redirty_instantiations);
        recover_corrupt_database(report);
        co_return report;
    }

    co_await migrate_shard_views(report);

    LOG_PERF("index",
             "phase=save shards={} manifests={} total={} elapsed_ms={}",
             shard_count,
             manifest_count,
             project.index.shards.size(),
             timer.ms());
    co_return report;
}

bool IndexStore::search_rebuild_due(SearchRebuild search) const {
    if(search == SearchRebuild::Never) {
        return false;
    }
    auto& index = project.index.search_index;
    auto base = index.size();
    if(project.index.search_pending.size() > std::max<std::size_t>(10000, base / 20)) {
        return true;
    }
    // A damaged index is replaced at the first save, idle or not: its
    // entries may misdescribe the table until then.
    if(index.damaged()) {
        return true;
    }
    if(search == SearchRebuild::Behind) {
        return false;
    }
    // A missing one — never built, or persisted under another pin than
    // the table's — leaves searches scanning the whole table until the
    // first idle save.
    if(!index.loaded()) {
        return project.index.symbol_count() > 0;
    }
    // A twentieth of the units, so a small project refreshes on any
    // merge and a large one every twenty at most.
    return merges_since_search_build >
           std::min<std::size_t>(20, project.index.manifests.size() / 20);
}

kota::task<> IndexStore::rebuild_search_index() {
    auto& project_index = project.index;
    ScopedTimer timer;
    index::SearchSnapshot snapshot;
    snapshot.entries.reserve(project_index.symbol_count());
    // A rebuild dirties the table (below): the save writes the project table
    // under its next generation, and the search blob is pinned to it.
    snapshot.generation = project_index.table_generation + 1;
    llvm::DenseMap<std::uint32_t, std::uint32_t> path_index;
    project_index.for_each_symbol(
        [&](index::SymbolHash hash, const index::SymbolView& symbol, std::uint32_t references) {
            if(!index::is_searchable_kind(symbol.kind) || symbol.name.empty()) {
                return true;
            }
            auto file = index::no_file;
            if(symbol.file != index::no_file) {
                auto [it, inserted] =
                    path_index.try_emplace(symbol.file,
                                           static_cast<std::uint32_t>(snapshot.paths.size()));
                if(inserted) {
                    snapshot.paths.push_back(project.index.portable(
                        project.file_table.resolve(Fid{symbol.file})));
                }
                file = it->second;
            }
            snapshot.entries.push_back({
                .hash = hash,
                .name = symbol.name.str(),
                .args = symbol.args.str(),
                .parent = symbol.parent,
                .kind = symbol.kind,
                .flags = symbol.flags,
                .file = file,
                .reference_files = references,
            });
            return true;
        });
    // The symbols of internal linkage are in no table but the shards of
    // the files declaring them.
    for(auto& [file, shard]: project_index.shards) {
        if(!shard.loaded()) {
            continue;
        }
        shard.for_each_local_symbol([&](index::SymbolHash hash,
                                        const index::SymbolView& symbol) {
            if(symbol.scope != index::SymbolScope::Internal ||
               !index::is_searchable_kind(symbol.kind) || symbol.name.empty() ||
               !index::declares(shard, hash)) {
                return true;
            }
            auto [path, inserted] =
                path_index.try_emplace(file.raw, static_cast<std::uint32_t>(snapshot.paths.size()));
            if(inserted) {
                snapshot.paths.push_back(project_index.portable(project.file_table.resolve(file)));
            }
            snapshot.entries.push_back({
                .hash = hash,
                .name = symbol.name.str(),
                .args = symbol.args.str(),
                .parent = symbol.parent,
                .kind = symbol.kind,
                .flags = symbol.flags,
                .file = path->second,
                .local = true,
            });
            return true;
        });
    }
    auto merges_in_snapshot = merges_since_search_build;
    // The entries pending now are in the snapshot; the ones a merge changes
    // during the build stay pending once it is adopted.
    llvm::SmallVector<index::SymbolHash> pending_in_snapshot(project_index.search_pending.begin(),
                                                             project_index.search_pending.end());
    llvm::SmallVector<Fid> pending_files_in_snapshot(project_index.search_pending_files.begin(),
                                                     project_index.search_pending_files.end());
    changed_during_search_build.emplace();
    auto finished = llvm::make_scope_exit([&] { changed_during_search_build.reset(); });

    std::string bytes;
    co_await kota::queue([&] { bytes = index::build_search_blob(snapshot); });
    index::SearchIndex built;
    if(!built.load(llvm::MemoryBuffer::getMemBufferCopy(bytes))) {
        LOG_ERROR("The rebuilt search index does not load; keeping the previous one");
        co_return;
    }
    project.index.search_index = std::move(built);
    // The table pins the blob it was saved with: a reader adopts the
    // persisted search index only under this generation.
    project.index.search_generation = snapshot.generation;
    table_dirty = true;
    merges_since_search_build -= merges_in_snapshot;
    for(auto hash: pending_in_snapshot) {
        if(project_index.search_index.contains(hash) &&
           !changed_during_search_build->symbols.contains(hash)) {
            project_index.search_pending.erase(hash);
        }
    }
    for(auto file: pending_files_in_snapshot) {
        if(!changed_during_search_build->files.contains(file)) {
            project_index.search_pending_files.erase(file);
        }
    }
    search_bytes = std::move(bytes);
    LOG_PERF("index",
             "phase=search_build symbols={} bytes={} elapsed_ms={}",
             project.index.search_index.size(),
             search_bytes.size(),
             timer.ms());
}

kota::task<> IndexStore::migrate_shard_views(Report& report) {
    if(!project.database) {
        co_return;
    }
    auto& db = *project.database;

    // A full-map write left nothing committed (everything is dirty again);
    // growing retires every snapshot at once, so the rebind below must run
    // to completion before the first yield.
    auto grown = db.grow();
    if(!grown) {
        // Degenerate (address space exhausted): borrowed views may already
        // be dead, so everything borrowed is shed rather than left
        // dangling. The shed shards' persisted bytes stay intact but their
        // manifests still read fresh — only the owner requeue rebuilds
        // their resident shards this session.
        LOG_ERROR("Index database growth failed: {}", grown.error());
        if(db.corrupted()) {
            recover_corrupt_database(report);
        } else {
            shed_borrowed_shards(report);
        }
        co_return;
    }
    bool grew = *grown;
    if(grew) {
        // The refused batch is dirty again; the grown map takes it now.
        report.owes_retry = true;
    } else {
        // The LMDB backend hands out pointers into its resident read
        // snapshot; after a commit the resident shards migrate onto a
        // fresh snapshot so the old one can be retired. Both snapshots
        // stay valid across the yields and the blobs are byte-identical,
        // so queries between batches may observe a mix of old and new
        // pointers with identical meaning. Shards whose write just failed
        // are dirty again by now (their bytes never committed) and stay
        // memory-backed until a later save.
        auto advanced = db.advance_read_snapshot();
        if(!advanced) {
            LOG_WARN("Index read-snapshot advance failed: {}", advanced.error());
            if(db.corrupted()) {
                recover_corrupt_database(report);
            }
            co_return;
        }
        if(*advanced == 0) {
            // Filesystem backend: buffers are immortal, nothing to migrate.
            co_return;
        }
    }

    constexpr std::size_t rebind_batch = 512;
    llvm::SmallVector<Fid> resident;
    for(auto path_id: llvm::make_first_range(project.index.shards)) {
        if(!dirty_shards.contains(path_id)) {
            resident.push_back(path_id);
        }
    }
    for(std::size_t i = 0; i < resident.size(); i += 1) {
        if(!grew && i != 0 && i % rebind_batch == 0) {
            co_await kota::yield(loop);
        }
        auto path_id = resident[i];
        auto it = project.index.shards.find(path_id);
        if(it == project.index.shards.end() || dirty_shards.contains(path_id)) {
            continue;
        }
        auto blob = db.read(index::BlobKind::Shard,
                            project.index.key_of(project.file_table, path_id));
        if(!blob || !it->second.rebind(std::move(blob.buffer))) {
            // Corruption can also show up first here (a damaged page only
            // this re-read reaches); the recovery below sheds the whole
            // resident set, nothing per-shard to do.
            if(db.corrupted()) {
                break;
            }
            // Unreachable under the writer lock; the shard is dropped and
            // its owners requeued to rebuild the shard, while keeping the
            // old view would dangle once the snapshot retires.
            LOG_ERROR("Index shard for {} diverged during snapshot migration",
                      project.file_table.resolve(path_id));
            assert(false && "persisted shard must survive snapshot migration");
            project.index.shards.erase(path_id);
            report.add_index_changed(path_id);
            requeue_owners(path_id, report);
        }
    }
    llvm::SmallVector<Fid> resident_instantiations;
    for(auto path_id: llvm::make_first_range(project.index.instantiations)) {
        if(!dirty_instantiations.contains(path_id)) {
            resident_instantiations.push_back(path_id);
        }
    }
    for(std::size_t i = 0; i < resident_instantiations.size() && !db.corrupted(); i += 1) {
        if(!grew && i != 0 && i % rebind_batch == 0) {
            co_await kota::yield(loop);
        }
        auto path_id = resident_instantiations[i];
        auto it = project.index.instantiations.find(path_id);
        if(it == project.index.instantiations.end() ||
           dirty_instantiations.contains(path_id)) {
            continue;
        }
        auto blob = db.read(index::BlobKind::InstantiationShard,
                            project.index.key_of(project.file_table, path_id));
        if(!blob || !it->second.rebind(std::move(blob.buffer))) {
            if(db.corrupted()) {
                break;
            }
            LOG_ERROR("Index instantiations for {} diverged during snapshot migration",
                      project.file_table.resolve(path_id));
            assert(false && "persisted instantiations must survive snapshot migration");
            project.index.instantiations.erase(path_id);
            report.add_index_changed(path_id);
            requeue_owners(path_id, report);
        }
    }
    // Corruption observed by any read since the write-time check — this
    // loop's, or a query's during its yields — condemns the database; the
    // recovery sheds every borrowed view before the environment closes.
    if(db.corrupted()) {
        recover_corrupt_database(report);
        co_return;
    }
    db.retire_old_snapshot();
}

void IndexStore::requeue_owners(Fid path_id, Report& report) {
    auto it = project.index.contributions.find(path_id);
    if(it == project.index.contributions.end()) {
        return;
    }
    for(auto tu: llvm::make_first_range(it->second)) {
        report.add_reindex(tu);
    }
}

void IndexStore::shed_borrowed_shards(Report& report) {
    llvm::SmallVector<Fid> shed;
    for(auto path_id: llvm::make_first_range(project.index.shards)) {
        if(!dirty_shards.contains(path_id)) {
            shed.push_back(path_id);
        }
    }
    for(auto path_id: shed) {
        project.index.shards.erase(path_id);
        report.add_index_changed(path_id);
        requeue_owners(path_id, report);
    }
    // Their users are the TUs including the file, which contribute to it.
    llvm::SmallVector<Fid> shed_instantiations;
    for(auto path_id: llvm::make_first_range(project.index.instantiations)) {
        if(!dirty_instantiations.contains(path_id)) {
            shed_instantiations.push_back(path_id);
        }
    }
    for(auto path_id: shed_instantiations) {
        project.index.instantiations.erase(path_id);
        report.add_index_changed(path_id);
        requeue_owners(path_id, report);
    }
}

void IndexStore::recover_corrupt_database(Report& report) {
    LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
    saved_shards = 0;
    shed_borrowed_shards(report);
    for(auto tu_path_id: llvm::make_first_range(project.index.manifests)) {
        dirty_manifests.insert(tu_path_id);
    }
    table_dirty = true;
    cdb_dirty = true;
    // The snapshot just persisted (if any) predates this recovery's debt
    // and lives in a condemned database anyway: one metadata retry makes
    // the fresh database record it.
    report.owes_retry = true;
    persisted_cdb_snapshot.clear();
    reopen_fresh_database();
}

void IndexStore::reopen_fresh_database() {
    project.database->condemn();
    project.database.reset();
    project.database = index::open_database(*project.store, project.build.active_configuration());
    // The metadata blobs died with the condemned database while their
    // loaded state lives on in memory; without a re-dirty the next save
    // skips them and a restart loses the user's context choices and every
    // rebuildable artifact record.
    project.mark_artifacts_dirty();
    contexts.dirty = !contexts.bytes.empty();
    // Durability waiters re-evaluate against the new database: a failed
    // reopen disables persistence for the session, and a parked
    // switchContext would otherwise sleep forever — no later save pulses,
    // they all early-return on the null database.
    contexts.committed.set();
    contexts.committed.reset();
}

IndexStore::LoadResult IndexStore::load(IndexLoadOptions options) {
    bool read_only = options.read_only;
    LoadResult result;
    auto& report = result.report;
    if(!project.database)
        return result;
    auto& db = *project.database;
    auto& project_index = project.index;
    ScopedTimer timer;

    auto remove_all = [&] {
        if(read_only) {
            return;
        }
        for(auto kind: {index::BlobKind::Shard,
                        index::BlobKind::InstantiationShard,
                        index::BlobKind::TUManifest,
                        index::BlobKind::SearchIndex}) {
            db.for_each_key(kind, [&](llvm::StringRef key) {
                startup_removes.push_back({kind, key.str()});
            });
        }
    };

    // Everything a read-only session keeps is deserialized or copied, so
    // nothing borrows the opening snapshot; retiring it keeps a long
    // read-only session (batch lint) from pinning a concurrent writer's
    // page reclamation to session start. Every return path that leaves
    // the database open owes this.
    auto retire_snapshot = [&] {
        if(read_only && !options.borrow && project.database && db.advance_read_snapshot()) {
            db.retire_old_snapshot();
        }
    };

    // Artifact metadata and context choices are index-independent: they
    // load (and self-validate) whatever happened to the project table.
    auto load_metadata = [&] {
        if(auto artifacts = db.read(index::BlobKind::ArtifactRecords, "artifacts")) {
            load_artifacts(artifacts.buffer->getBuffer());
        }
        if(auto choices = db.read(index::BlobKind::ContextChoices, "contexts")) {
            contexts.bytes = choices.buffer->getBuffer().str();
        }
    };

    auto table = db.read(index::BlobKind::ProjectTable, "global");
    if(!table) {
        // A project table blob that exists but failed to open is a transient IO
        // error, not absence: removal would destroy an intact index and
        // force a full rebuild. Run this session memory-only instead —
        // saving a fresh lineage over blobs whose anchor was never read
        // could alias their fv ids and generations — and leave
        // everything for a healthier restart to load.
        if(db.contains(index::BlobKind::ProjectTable, "global")) {
            // Confirmed page corruption heals through rebuildability: the
            // condemned database deletes itself and a fresh empty one
            // opens in its place, so this session's rebuild persists
            // instead of being redone at the next start. Transient
            // failures touch nothing.
            if(!read_only && db.corrupted()) {
                LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
                reopen_fresh_database();
            } else {
                // Best effort before detaching: the metadata blobs may
                // still read while only the project table is unreadable.
                load_metadata();
                LOG_WARN("Index project table unreadable; disabling index persistence this session");
                project.database.reset();
            }
            return result;
        }
        // No project table means no resolvable manifests: everything else
        // is unreachable data, removed so it cannot survive as orphans.
        remove_all();
        load_metadata();
        retire_snapshot();
        return result;
    }
    llvm::DenseMap<VersionID, std::uint64_t> manifest_pins;
    if(auto loaded =
           project_index.load_table(table.buffer->getBuffer(), project.file_table, manifest_pins);
       !loaded) {
        LOG_INFO("Discarding the index's project table: {}", loaded.error());
        remove_all();
        load_metadata();
        if(!read_only) {
            startup_removes.push_back({index::BlobKind::ProjectTable, "global"});
            startup_removes.push_back({index::BlobKind::SearchIndex, "search"});
        }
        result.decoded = false;
        retire_snapshot();
        return result;
    }
    load_metadata();

    // The search index is derived from the table just loaded: adopt the
    // persisted one when the table pins it, else searches scan the table
    // until the next save rebuilds it. A borrowed reader keeps the
    // snapshot bytes; a writer copies, since its own saves retire
    // snapshots.
    if(auto search = db.read(index::BlobKind::SearchIndex, "search")) {
        if(!options.borrow && search.generation != 0) {
            search.buffer = llvm::MemoryBuffer::getMemBufferCopy(search.buffer->getBuffer());
        }
        if(!project_index.bind_search(std::move(search.buffer)) && !read_only) {
            startup_removes.push_back({index::BlobKind::SearchIndex, "search"});
        }
    }

    // Adopt exactly the manifests the project table pins, at exactly the
    // pinned generation and with every FileVersion resolvable. The
    // rest are stale residue — a crash between batch phases, a failed
    // write under a committed project table, a dropped TU whose removal was lost —
    // and are removed, with their TUs re-enqueued where recoverable.
    llvm::DenseSet<VersionID> adopted_pins;
    llvm::SmallVector<std::string> dead_manifests;
    db.for_each_key(index::BlobKind::TUManifest, [&](llvm::StringRef key) {
        auto blob = db.read(index::BlobKind::TUManifest, key);
        auto manifest = blob ? index::deserialize_manifest(blob.buffer->getBuffer()) : std::nullopt;
        if(!manifest) {
            dead_manifests.push_back(key.str());
            return;
        }
        auto tu_fv = project_index.runtime_version(manifest->tu_fv.raw);
        bool imported = project_index.import_manifest(*manifest);
        auto pin = imported ? manifest_pins.find(manifest->tu_fv) : manifest_pins.end();
        if(pin == manifest_pins.end() || pin->second != manifest->table_generation) {
            dead_manifests.push_back(key.str());
            // The manifest raced a crash ahead of the project table (its own
            // pin never committed). When the TU's version is still resolvable,
            // re-enqueue it: the CDB pass never covers standalone-indexed
            // headers.
            if(tu_fv) {
                report.add_reindex(project.file_table.version(*tu_fv).fid);
            }
            return;
        }
        adopted_pins.insert(manifest->tu_fv);
        auto tu_path_id = project.file_table.version(manifest->tu_fv).fid;
        project_index.apply_manifest(project.file_table, tu_path_id, std::move(*manifest));
    });
    if(!read_only) {
        for(auto& key: dead_manifests) {
            startup_removes.push_back({index::BlobKind::TUManifest, std::move(key)});
        }
    }
    // A pinned TU without an adopted manifest lost it to a failed write
    // that the committed project table outran, or to a lost removal; re-enqueue it —
    // its index is unservable until a reindex. Pinned fvs always resolve:
    // load_table rejects a blob whose pins its own table cannot cover.
    for(auto fv: llvm::make_first_range(manifest_pins)) {
        if(!adopted_pins.contains(fv)) {
            report.add_reindex(project.file_table.version(fv).fid);
        }
    }

    // Every contributing FileVersion pins the content generation its variant
    // was built from; the shard must store that same generation. The
    // variant check below cannot catch a stale shard alone when an edit
    // past every indexed entry left the variant hash identical: all recorded
    // versions would match the disk while positions map through the old
    // text, forever. A version with no consumed-content hash (0) pins
    // nothing — it is permanently stale and reindexes its TU anyway.
    llvm::DenseMap<Fid, llvm::SmallVector<std::uint64_t, 1>> generations;
    for(auto& manifest: llvm::make_second_range(project_index.manifests)) {
        for(auto fv: llvm::make_first_range(manifest.contributions)) {
            auto& record = project.file_table.version(fv);
            if(record.content_hash == 0) {
                continue;
            }
            auto& pinned = generations[record.fid];
            if(!llvm::is_contained(pinned, record.content_hash)) {
                pinned.push_back(record.content_hash);
            }
        }
    }

    // Fetch exactly the shard blobs the contributions expect. A blob that
    // is missing or fails verification leaves its contributing TUs' variants
    // unservable, so those manifests are dropped and the TUs reindex (for
    // headers no CDB entry would ever re-enqueue them otherwise).
    llvm::StringSet<> expected_keys;
    llvm::SmallVector<Fid> unservable;
    for(auto& [path_id, entry]: project_index.contributions) {
        auto key = project.index.key_of(project.file_table, path_id);
        auto blob = db.read(index::BlobKind::Shard, key);
        if(read_only && !options.borrow && blob && blob.generation != 0) {
            // A read-only session never advances snapshots, so borrowed
            // bytes would pin the opening snapshot for its whole lifetime
            // while a concurrent writer churns; copies let the snapshot
            // retire right after the load.
            blob.buffer = llvm::MemoryBuffer::getMemBufferCopy(blob.buffer->getBuffer());
            blob.generation = 0;
        }
        auto shard = index::Shard::from_buffer(std::move(blob.buffer));
        // A blob can verify yet miss a contributed variant, or carry
        // another content generation than the contributions pin (crash or
        // failed write left a manifest newer than its shard); set_live
        // would drop missing variants silently and stale content misplaces
        // every position, so both are as unservable as an unreadable blob.
        auto generation_ok = [&] {
            auto it = generations.find(path_id);
            return it == generations.end() || llvm::all_of(it->second, [&](std::uint64_t hash) {
                       return hash == shard.content_hash();
                   });
        };
        bool servable = shard.loaded() && generation_ok() &&
                        llvm::all_of(llvm::make_second_range(entry),
                                     [&](std::uint64_t hash) { return shard.has_variant(hash); });
        if(!servable) {
            LOG_INFO("Discarding unservable shard for {}", project.file_table.resolve(path_id));
            unservable.push_back(path_id);
            continue;
        }
        expected_keys.insert(key);
        shard.set_live(project_index.live_variants(path_id));
        project.index.shards[path_id] = std::move(shard);
    }
    llvm::SmallVector<Fid> mask_refresh;
    for(auto path_id: unservable) {
        auto contribution_it = project_index.contributions.find(path_id);
        if(contribution_it == project_index.contributions.end()) {
            continue;
        }
        llvm::SmallVector<Fid> owners;
        for(auto tu: llvm::make_first_range(contribution_it->second)) {
            owners.push_back(tu);
        }
        for(auto tu: owners) {
            // The removal retires the TU's contributions to EVERY file it
            // touched, not just the unservable one; the affected set feeds
            // the mask refresh below, like the merge path's.
            auto affected = project_index.remove_manifest(project.file_table, tu);
            mask_refresh.append(affected.begin(), affected.end());
            if(!read_only) {
                startup_removes.push_back({index::BlobKind::TUManifest,
                                           project.index.key_of(project.file_table, tu)});
            }
            report.add_reindex(tu);
        }
    }
    for(auto path_id: mask_refresh) {
        refresh_live(path_id);
    }

    // Each loaded shard's instantiations, at its generation. A use some
    // manifest names that its file no longer stores lost what that TU's
    // instantiations resolved there: the TU reindexes, keeping its index
    // meanwhile.
    llvm::StringSet<> expected_instantiations;
    for(auto& [path_id, shard]: project.index.shards) {
        auto key = project.index.key_of(project.file_table, path_id);
        auto blob = db.read(index::BlobKind::InstantiationShard, key);
        if(!blob) {
            continue;
        }
        if(read_only && !options.borrow && blob.generation != 0) {
            blob.buffer = llvm::MemoryBuffer::getMemBufferCopy(blob.buffer->getBuffer());
        }
        auto instantiations = index::InstantiationShard::from_buffer(std::move(blob.buffer));
        // A stored blob names the run behind each key; a worker's names none.
        auto keys = instantiations.keys();
        if(!instantiations.loaded() || instantiations.content_hash() != shard.content_hash() ||
           (!keys.empty() && !instantiations.find_key(keys.front())->producer)) {
            LOG_INFO("Discarding unservable instantiations for {}",
                     project.file_table.resolve(path_id));
            continue;
        }
        expected_instantiations.insert(key);
        instantiations.set_live(project_index.live_uses(path_id));
        project_index.instantiations[path_id] = std::move(instantiations);
    }
    for(auto& [tu, manifest]: project_index.manifests) {
        if(llvm::any_of(manifest.instantiations, [&](const index::InstantiationUse& use) {
               auto it =
                   project_index.instantiations.find(project.file_table.version(use.file).fid);
               return it == project_index.instantiations.end() || !it->second.has_use(use.use);
           })) {
            report.add_reindex(tu);
        }
    }

    // Remove shard blobs nothing references any more.
    if(!read_only) {
        db.for_each_key(index::BlobKind::Shard, [&](llvm::StringRef key) {
            if(!expected_keys.contains(key)) {
                startup_removes.push_back({index::BlobKind::Shard, key.str()});
            }
        });
        db.for_each_key(index::BlobKind::InstantiationShard, [&](llvm::StringRef key) {
            if(!expected_instantiations.contains(key)) {
                startup_removes.push_back({index::BlobKind::InstantiationShard, key.str()});
            }
        });
        retire_excluded(report);
        reconcile_cdb_snapshot(report);
    }

    // The reads above touch every adopted manifest and shard, so page
    // corruption anywhere in the database has latched by now; heal it like
    // the unreadable-table case above instead of writing into a damaged
    // tree every session. The adopted state unwinds wholesale: the shard
    // views borrow from the condemned environment, and manifests kept
    // without their views would read as fresh and gate the rebuild pass
    // off exactly the files whose shards were lost. Standalone-indexed TUs
    // re-enqueue first — the CDB pass that rebuilds everything else never
    // covers them, and the condemned database is deleting their only
    // persistent record. The dirtied snapshot carries them as debt from
    // the fresh database's first save on, so even a crash before their
    // rebuild finishes cannot lose them a second time.
    if(!read_only && db.corrupted()) {
        LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
        for(auto tu: llvm::make_first_range(project_index.manifests)) {
            if(project.build.entries(tu).empty()) {
                report.add_reindex(tu);
            }
        }
        project.index.shards.clear();
        project_index = index::ProjectIndex();
        startup_removes.clear();
        persisted_cdb_snapshot.clear();
        cdb_dirty = true;
        reopen_fresh_database();
        return result;
    }

    retire_snapshot();

    if(!project.index.shards.empty()) {
        LOG_INFO("Loaded {} index shards, {} manifests, {} symbols",
                 project.index.shards.size(),
                 project_index.manifests.size(),
                 project_index.symbol_count());
    }
    LOG_PERF("startup",
             "phase=index_load symbols={} shards={} manifests={} elapsed_ms={}",
             project_index.symbol_count(),
             project.index.shards.size(),
             project_index.manifests.size(),
             timer.ms());
    return result;
}

llvm::SmallVector<Fid> IndexStore::standalone_of(llvm::ArrayRef<Fid> candidates) {
    llvm::SmallVector<Fid> debt;
    llvm::DenseSet<Fid> seen;
    for(auto id: candidates) {
        if(project.index.manifests.contains(id) || !project.build.entries(id).empty() ||
           !seen.insert(id).second) {
            continue;
        }
        debt.push_back(id);
    }
    return debt;
}

void IndexStore::retire_excluded(Report& report) {
    llvm::SmallVector<Fid> excluded;
    for(auto tu: llvm::make_first_range(project.index.manifests)) {
        if(!project.build.indexed(project.file_table.resolve(tu))) {
            excluded.push_back(tu);
        }
    }
    for(auto tu: excluded) {
        LOG_INFO("A rule keeps {} out of the index; dropping its rows",
                 project.file_table.resolve(tu));
        drop_index_into(tu, report);
    }
}

IndexStore::PersistedBuild IndexStore::remembered() {
    PersistedBuild result;
    if(!project.database) {
        return result;
    }
    auto blob = project.database->read(index::BlobKind::CommandSnapshot, "cdb");
    CommandSnapshot persisted;
    if(!blob ||
       !kota::codec::json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        return result;
    }
    for(auto& entry: persisted.entries) {
        for(auto& source: entry.sources) {
            auto absolute = project.index.local(source);
            if(!llvm::is_contained(result.sources, absolute)) {
                result.sources.push_back(std::move(absolute));
            }
        }
    }
    for(auto& file: persisted.provisional) {
        result.provisional.push_back(project.index.local(file));
    }
    return result;
}

void IndexStore::reconcile_cdb_snapshot(Report& report) {
    auto blob = project.database->read(index::BlobKind::CommandSnapshot, "cdb");
    if(!blob) {
        // Unknown baseline: nothing to diff against. Dirty the snapshot so
        // the next save recreates it even when it commits nothing else —
        // after a crash that lost only the CDB blob, waiting for an
        // unrelated dirtying merge would leave offline command edits
        // undetectable across every following session.
        cdb_dirty = true;
        return;
    }
    CommandSnapshot persisted;
    if(!kota::codec::json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        LOG_ERROR("Index cache at {} cannot tell which commands built it; reindexing every file",
                  std::string_view(project.config.project.cache_dir));
        cdb_dirty = true;
        llvm::SmallVector<Fid> units(llvm::make_first_range(project.index.manifests));
        for(auto unit: units) {
            drop_index_into(unit, report);
            report.add_reindex(unit);
        }
        return;
    }
    persisted_cdb_snapshot = blob.buffer->getBuffer().str();

    llvm::StringMap<const CDBSnapshotEntry*> before;
    for(auto& entry: persisted.entries) {
        before[entry.file] = &entry;
    }
    auto& project_index = project.index;
    auto file_of = [&](llvm::StringRef name) {
        return project.file_table.intern(project_index.local(name));
    };
    llvm::DenseSet<Fid> cdb_ids;
    llvm::SmallVector<Fid> changed_ids;
    auto snapshot = build_cdb_snapshot(project, header_hosts, {});
    for(auto& entry: snapshot.entries) {
        if(entry.hashes.empty()) {
            continue;
        }
        auto server_id = file_of(entry.file);
        cdb_ids.insert(server_id);
        auto it = before.find(entry.file);
        // `selected` guards the offline winner flip: the candidate multiset
        // can survive a reload that still changes which entry is the
        // default selection.
        if(it != before.end() && it->second->hashes == entry.hashes &&
           it->second->selected == entry.selected && it->second->rules == entry.rules &&
           it->second->sources == entry.sources) {
            continue;
        }
        changed_ids.push_back(server_id);
        if(!project_index.manifests.contains(server_id)) {
            continue;
        }
        LOG_INFO("Compile command changed since the last session; reindexing {}",
                 project.file_table.resolve(server_id));
        drop_index_into(server_id, report);
        report.add_reindex(server_id);
    }

    // A standalone-indexed header borrowed a host source's command and
    // applied its own matched rules on top, so an offline change to either
    // staled its index exactly like the live CDB path's hosted-header
    // invalidation. A header whose recorded host survives unchanged and
    // still includes it is pinned fresh; one with no recorded host (older
    // snapshot) falls back to the include-reachability approximation below.
    llvm::DenseSet<Fid> pinned_fresh;
    llvm::DenseSet<Fid> retired;
    for(auto& entry: snapshot.entries) {
        if(!entry.hashes.empty()) {
            continue;
        }
        auto it = before.find(entry.file);
        if(it == before.end()) {
            // Not in the persisted snapshot: the next save adopts it, and
            // offline changes against an unknown baseline are undetectable
            // anyway.
            continue;
        }
        auto& old = *it->second;
        auto server_id = file_of(entry.file);
        if(!old.hashes.empty()) {
            // Its entries vanished. A default command that still covers it
            // is a command change; otherwise the retirement pass below
            // decides whether the index leaves.
            if(!entry.selected.empty()) {
                LOG_INFO("Compile command changed since the last session; reindexing {}",
                         project.file_table.resolve(server_id));
                drop_index_into(server_id, report);
                report.add_reindex(server_id);
            }
            continue;
        }
        // Hosts are only known once a header has been resolved this
        // session; until then the recorded host stands in, or every host
        // edit would read as a rule change at each start.
        std::string rules = entry.rules;
        std::string host_selected = entry.host_selected;
        if(entry.host.empty() && !old.host.empty()) {
            auto host_id = file_of(old.host);
            CanonicalRef paths[] = {project.file_table.resolve(host_id),
                                    project.file_table.resolve(server_id)};
            rules = project.build.edit_hash(paths);
            host_selected = selected_hash(project, host_id);
        }
        if(old.rules != rules || old.selected != entry.selected) {
            // The command that covered it — a rule's default command, or the
            // one a provisional member borrowed — is gone and no host
            // confirms it: the build stopped compiling it, so its index
            // leaves rather than being rebuilt under the builtin fallback.
            if(!old.selected.empty() && entry.selected.empty() && old.host.empty()) {
                LOG_INFO("The build no longer compiles {}; dropping its index",
                         project.file_table.resolve(server_id));
                drop_index_into(server_id, report);
                retired.insert(server_id);
                continue;
            }
            LOG_INFO("Compile command or rules changed since the last session; reindexing {}",
                     project.file_table.resolve(server_id));
            drop_index_into(server_id, report);
            report.add_reindex(server_id);
            changed_ids.push_back(server_id);
            continue;
        }
        if(old.host.empty()) {
            continue;
        }
        auto host_id = file_of(old.host);
        if(project.build.commands(host_id).empty() || llvm::is_contained(changed_ids, host_id) ||
           old.host_selected != host_selected) {
            LOG_INFO("Host compile command changed since the last session; reindexing {}",
                     project.file_table.resolve(server_id));
            drop_index_into(server_id, report);
            report.add_reindex(server_id);
            continue;
        }
        // The dependency scan preceding this load saw the offline edits, so
        // a recorded host that the build no longer ranks first — it stopped
        // including the header, or a rule change moved another database
        // ahead — cannot confirm the borrowed command any more. Keep the
        // index serving (last known good, like the vanished-entry case
        // above) while a rebuild re-selects a host; a Fallback resolution
        // then changes nothing. The retained index was still built through
        // the old host, so keep that association until a finished rebuild
        // overwrites it — an empty host persisted after a Fallback or
        // failed rebuild would hit the `old.host.empty()` gate next session
        // and never retry.
        auto current = default_host(project, server_id);
        if(!current || current->file != host_id) {
            LOG_INFO("Default host of {} changed since the last session; reindexing",
                     project.file_table.resolve(server_id));
            header_hosts[server_id] = host_id;
            report.add_reindex(server_id);
            continue;
        }
        header_hosts[server_id] = host_id;
        pinned_fresh.insert(server_id);
    }

    // A file no database lists any more: when every database that listed
    // it loaded fine this session, the build stopped compiling it and its
    // index leaves, as the live reload's removed branch does; a database
    // that failed to load keeps its last-known entries serving. One the
    // configuration stopped declaring is gone on purpose; one discovery no
    // longer finds may come back.
    bool declared = project.build.declares_sources();
    // Under discovery a database that is no longer registered was either
    // replaced by one that loaded, or merely vanished and may come back.
    bool replaced = false;
    for(std::uint32_t i = 0; !declared && i < project.cdb.source_count(); i += 1) {
        replaced |= project.cdb.loaded(SourceID(i));
    }
    for(auto& old: persisted.entries) {
        if(old.hashes.empty() || old.sources.empty()) {
            continue;
        }
        auto server_id = file_of(old.file);
        if(!project.build.commands(server_id).empty()) {
            continue;
        }
        bool healthy = llvm::all_of(old.sources, [&](const std::string& source) {
            auto id = project.cdb.find_source(project_index.local(source));
            return id ? project.cdb.loaded(*id) : declared || replaced;
        });
        if(!healthy) {
            continue;
        }
        LOG_INFO("No compilation database lists {} any more; dropping its index",
                 project.file_table.resolve(server_id));
        drop_index_into(server_id, report);
    }

    // A persisted standalone entry whose file has no manifest is recorded
    // debt: its index was dropped for a command or rule change and no
    // rebuild has finished since — with no manifest pin and no CDB entry,
    // nothing else would ever retry it. Re-enqueue while the file exists;
    // a vanished file's debt dies with its entry at the next save.
    for(auto& old: persisted.entries) {
        if(!old.hashes.empty() || !project.build.entries(file_of(old.file)).empty()) {
            continue;
        }
        auto server_id = file_of(old.file);
        if(retired.contains(server_id) || project_index.manifests.contains(server_id) ||
           !vfs::exists(project.file_table.resolve(server_id))) {
            continue;
        }
        LOG_INFO("Index owed from the last session; reindexing {}",
                 project.file_table.resolve(server_id));
        report.add_reindex(server_id);
    }
    if(changed_ids.empty()) {
        return;
    }

    llvm::SmallVector<Fid> hosted;
    for(auto tu: llvm::make_first_range(project_index.manifests)) {
        if(cdb_ids.contains(tu) || pinned_fresh.contains(tu)) {
            continue;
        }
        if(llvm::any_of(changed_ids, [&](Fid host) {
               return !project.dep_graph.find_include_chain(host, tu).empty();
           })) {
            hosted.push_back(tu);
        }
    }
    for(auto header_id: hosted) {
        LOG_INFO("Host compile command changed since the last session; reindexing {}",
                 project.file_table.resolve(header_id));
        drop_index_into(header_id, report);
        report.add_reindex(header_id);
    }
}

bool IndexStore::file_version_stale(VersionID fv_id) {
    auto [cached, inserted] = fv_stale.try_emplace(fv_id, true);
    if(!inserted) {
        return cached->second;
    }

    // Missing and unreadable both read as stale — conservative, the
    // reindex re-observes.
    bool stale = project.file_table.check_version(fv_id) != vfs::DiskState::CheckResult::Fresh;
    fv_stale[fv_id] = stale;
    return stale;
}

bool IndexStore::needs_reindex(Fid file) {
    auto& project_index = project.index;
    auto manifest_it = project_index.manifests.find(file);
    if(manifest_it == project_index.manifests.end())
        return true;

    // Every referenced version must be validated: whichever a partial
    // iteration skipped would keep serving a stale index behind a fresh
    // result.
    auto& manifest = manifest_it->second;
    if(file_version_stale(manifest.tu_fv)) {
        return true;
    }
    for(auto& node: manifest.nodes) {
        if(file_version_stale(VersionID{node.file})) {
            return true;
        }
    }
    if(llvm::any_of(manifest.imports, [&](VersionID fv) { return file_version_stale(fv); })) {
        return true;
    }
    return llvm::any_of(manifest.absent, [&](VersionID fv) {
        return project.file_table.disk.present(project.file_table.version(fv).fid);
    });
}

}  // namespace clice
