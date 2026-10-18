module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.instantiation_shard;
import :index.project_index;
import :index.shard;
import :index.tu_manifest;
import :support.logging;

namespace clice::index {

LoadOutcome ProjectIndex::load(BlobDatabase& db, LoadOptions options, IndexReport& report) {
    bool read_only = options.read_only;

    auto remove_all = [&] {
        if(read_only) {
            return;
        }
        for(auto kind: {BlobKind::Shard,
                        BlobKind::InstantiationShard,
                        BlobKind::TUManifest,
                        BlobKind::SearchIndex}) {
            db.for_each_key(kind, [&](llvm::StringRef key) {
                pending_removals.push_back({kind, key.str()});
            });
        }
    };

    auto table = db.read(BlobKind::ProjectTable, "global");
    if(!table) {
        // A project table blob that exists but failed to open is a transient IO
        // error, not absence: removal would destroy an intact index and
        // force a full rebuild.
        if(db.contains(BlobKind::ProjectTable, "global")) {
            return LoadOutcome::Unreadable;
        }
        // No project table means no resolvable manifests: everything else
        // is unreachable data, removed so it cannot survive as orphans.
        remove_all();
        return LoadOutcome::Empty;
    }
    llvm::DenseMap<VersionID, std::uint64_t> manifest_pins;
    if(auto loaded = load_table(table.buffer->getBuffer(), manifest_pins); !loaded) {
        LOG_INFO("Discarding the index's project table: {}", loaded.error());
        remove_all();
        if(!read_only) {
            pending_removals.push_back({BlobKind::ProjectTable, "global"});
            pending_removals.push_back({BlobKind::SearchIndex, "search"});
        }
        return LoadOutcome::Undecodable;
    }

    // The search index is derived from the table just loaded: adopt the
    // persisted one when the table pins it, else searches scan the table
    // until the next save rebuilds it. A borrowed reader keeps the
    // snapshot bytes; a writer copies, since its own saves retire
    // snapshots.
    if(auto blob = db.read(BlobKind::SearchIndex, "search")) {
        if(!options.borrow && blob.generation != 0) {
            blob.buffer = llvm::MemoryBuffer::getMemBufferCopy(blob.buffer->getBuffer());
        }
        if(!bind_search(std::move(blob.buffer)) && !read_only) {
            pending_removals.push_back({BlobKind::SearchIndex, "search"});
        }
    }

    // Adopt exactly the manifests the project table pins, at exactly the
    // pinned generation and with every FileVersion resolvable. The
    // rest are stale residue — a crash between batch phases, a failed
    // write under a committed project table, a dropped TU whose removal was lost —
    // and are removed, with their TUs re-enqueued where recoverable.
    llvm::DenseSet<VersionID> adopted_pins;
    llvm::SmallVector<std::string> dead_manifests;
    db.for_each_key(BlobKind::TUManifest, [&](llvm::StringRef key) {
        auto blob = db.read(BlobKind::TUManifest, key);
        auto manifest = blob ? deserialize_manifest(blob.buffer->getBuffer()) : std::nullopt;
        if(!manifest) {
            dead_manifests.push_back(key.str());
            return;
        }
        auto tu_version = runtime_version(manifest->tu_version.raw);
        bool imported = import_manifest(*manifest);
        auto pin = imported ? manifest_pins.find(manifest->tu_version) : manifest_pins.end();
        if(pin == manifest_pins.end() || pin->second != manifest->table_generation) {
            dead_manifests.push_back(key.str());
            // The manifest raced a crash ahead of the project table (its own
            // pin never committed). When the TU's version is still resolvable,
            // re-enqueue it: the CDB pass never covers standalone-indexed
            // headers.
            if(tu_version) {
                report.add_reindex(files.version(*tu_version).fid);
            }
            return;
        }
        adopted_pins.insert(manifest->tu_version);
        auto tu_path_id = files.version(manifest->tu_version).fid;
        apply_manifest(tu_path_id, std::move(*manifest));
    });
    if(!read_only) {
        for(auto& key: dead_manifests) {
            pending_removals.push_back({BlobKind::TUManifest, std::move(key)});
        }
    }
    // A pinned TU without an adopted manifest lost it to a failed write
    // that the committed project table outran, or to a lost removal; re-enqueue it —
    // its index is unservable until a reindex. Pinned versions always resolve:
    // load_table rejects a blob whose pins its own table cannot cover.
    for(auto version: llvm::make_first_range(manifest_pins)) {
        if(!adopted_pins.contains(version)) {
            report.add_reindex(files.version(version).fid);
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
    for(auto& manifest: llvm::make_second_range(manifests)) {
        for(auto version: llvm::make_first_range(manifest.contributions)) {
            auto& record = files.version(version);
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
    for(auto& [path_id, entry]: contributions) {
        auto key = key_of(path_id);
        auto blob = db.read(BlobKind::Shard, key);
        if(read_only && !options.borrow && blob && blob.generation != 0) {
            // A read-only session never advances snapshots, so borrowed
            // bytes would pin the opening snapshot for its whole lifetime
            // while a concurrent writer churns; copies let the snapshot
            // retire right after the load.
            blob.buffer = llvm::MemoryBuffer::getMemBufferCopy(blob.buffer->getBuffer());
            blob.generation = 0;
        }
        auto shard = Shard::from_buffer(std::move(blob.buffer));
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
            LOG_INFO("Discarding unservable shard for {}", files.resolve(path_id));
            unservable.push_back(path_id);
            continue;
        }
        expected_keys.insert(key);
        shard.set_live(live_variants(path_id));
        shards[path_id] = std::move(shard);
    }
    for(auto path_id: unservable) {
        llvm::SmallVector<Fid> owners;
        for_each_contribution(path_id, [&](Fid tu, std::uint64_t) { owners.push_back(tu); });
        for(auto tu: owners) {
            // The removal retires the TU's contributions to EVERY file it
            // touched, not just the unservable one, and refreshes their
            // live masks like a merge.
            remove_manifest(tu);
            if(!read_only) {
                pending_removals.push_back({BlobKind::TUManifest, key_of(tu)});
            }
            report.add_reindex(tu);
        }
    }

    // Each loaded shard's instantiations, at its generation. A use some
    // manifest names that its file no longer stores lost what that TU's
    // instantiations resolved there: the TU reindexes, keeping its index
    // meanwhile.
    llvm::StringSet<> expected_instantiations;
    for(auto& [path_id, shard]: shards) {
        auto key = key_of(path_id);
        auto blob = db.read(BlobKind::InstantiationShard, key);
        if(!blob) {
            continue;
        }
        if(read_only && !options.borrow && blob.generation != 0) {
            blob.buffer = llvm::MemoryBuffer::getMemBufferCopy(blob.buffer->getBuffer());
        }
        auto loaded = InstantiationShard::from_buffer(std::move(blob.buffer));
        // A stored blob names the run behind each key; a worker's names none.
        auto keys = loaded.keys();
        if(!loaded.loaded() || loaded.content_hash() != shard.content_hash() ||
           (!keys.empty() && !loaded.find_key(keys.front())->producer)) {
            LOG_INFO("Discarding unservable instantiations for {}", files.resolve(path_id));
            continue;
        }
        expected_instantiations.insert(key);
        loaded.set_live(live_uses(path_id));
        instantiations[path_id] = std::move(loaded);
    }
    for(auto& [tu, manifest]: manifests) {
        if(llvm::any_of(manifest.instantiations, [&](const InstantiationUse& use) {
               auto it = instantiations.find(files.version(use.file).fid);
               return it == instantiations.end() || !it->second.has_use(use.use);
           })) {
            report.add_reindex(tu);
        }
    }

    // Remove shard blobs nothing references any more.
    if(!read_only) {
        db.for_each_key(BlobKind::Shard, [&](llvm::StringRef key) {
            if(!expected_keys.contains(key)) {
                pending_removals.push_back({BlobKind::Shard, key.str()});
            }
        });
        db.for_each_key(BlobKind::InstantiationShard, [&](llvm::StringRef key) {
            if(!expected_instantiations.contains(key)) {
                pending_removals.push_back({BlobKind::InstantiationShard, key.str()});
            }
        });
    }
    return LoadOutcome::Loaded;
}

void ProjectIndex::clear() {
    // Replaced in place: the file table reference keeps the type from
    // being assigned.
    auto& table = files;
    auto root = std::move(workspace);
    std::destroy_at(this);
    std::construct_at(this, table);
    workspace = std::move(root);
}

}  // namespace clice::index
