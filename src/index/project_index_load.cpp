module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.project_index;
import :index.tu_manifest;
import :support.logging;

namespace clice::index {

LoadOutcome ProjectIndex::load(BlobDatabase& db, LoadOptions options, IndexReport& report) {
    bool read_only = options.read_only;
    this->db = &db;
    borrow = !read_only || options.borrow;

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
    // A writer moves the table onto each snapshot its saves open
    // (migrate_views); a reader that does not borrow lets the snapshot
    // retire, and copies.
    auto bytes = borrow ? std::move(table.buffer)
                        : llvm::MemoryBuffer::getMemBufferCopy(table.buffer->getBuffer());
    if(auto loaded = load_table(std::move(bytes)); !loaded) {
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
    if(read_only) {
        return LoadOutcome::Loaded;
    }

    // The blobs no unit names are residue — a crash between batch phases, a
    // failed write under a committed project table, a dropped TU whose
    // removal was lost — and leave at the first save; the keys alone tell.
    llvm::StringSet<> expected;
    for_each_unit([&](Fid tu) { expected.insert(key_of(tu)); });
    db.for_each_key(BlobKind::TUManifest, [&](llvm::StringRef key) {
        if(expected.contains(key)) {
            return;
        }
        pending_removals.push_back({BlobKind::TUManifest, key.str()});
        // The manifest raced a crash ahead of the project table (its own
        // pin never committed). When the TU's version is still resolvable,
        // re-enqueue it: the CDB pass never covers standalone-indexed
        // headers.
        auto blob = db.read(BlobKind::TUManifest, key);
        auto manifest = blob ? deserialize_manifest(blob.buffer->getBuffer()) : std::nullopt;
        if(auto tu_version = manifest ? runtime_version(manifest->tu_version.raw) : std::nullopt) {
            report.add_reindex(files.version(*tu_version).fid);
        }
    });
    expected.clear();
    for(auto& contributed: base->columns->contributions) {
        expected.insert(key_of(base->remap[contributed.file]));
    }
    db.for_each_key(BlobKind::Shard, [&](llvm::StringRef key) {
        if(!expected.contains(key)) {
            pending_removals.push_back({BlobKind::Shard, key.str()});
        }
    });
    // A file's instantiations live with its shard.
    db.for_each_key(BlobKind::InstantiationShard, [&](llvm::StringRef key) {
        if(!expected.contains(key)) {
            pending_removals.push_back({BlobKind::InstantiationShard, key.str()});
        }
    });
    return LoadOutcome::Loaded;
}

void ProjectIndex::clear() {
    // Replaced in place: the file table reference keeps the type from
    // being assigned. The workspace and the lost-data subscribers stay.
    auto& table = files;
    auto root = std::move(workspace);
    auto lost = std::move(on_data_lost);
    std::destroy_at(this);
    std::construct_at(this, table);
    workspace = std::move(root);
    on_data_lost = std::move(lost);
    table_dirty = true;
}

}  // namespace clice::index
