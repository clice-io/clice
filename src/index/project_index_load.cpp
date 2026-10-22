module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.project_index;
import :index.project_records;
import :support.logging;

namespace clice::index {

LoadOutcome ProjectIndex::open(BlobDatabase& db) {
    auto blob = db.read(BlobKind::Header, "header");
    if(!blob) {
        if(db.contains(BlobKind::Header, "header")) {
            return LoadOutcome::Unreadable;
        }
        this->db = &db;
        return LoadOutcome::Empty;
    }
    auto decoded = decode_header(blob.buffer->getBuffer());
    if(!decoded) {
        LOG_INFO("The index database is of another format; discarding it");
        return LoadOutcome::Undecodable;
    }
    header = std::move(*decoded);
    this->db = &db;
    pending_symbols.insert(header.search_pending.begin(), header.search_pending.end());
    for(auto path: header.search_pending_files) {
        if(auto file = fid_of(path); file.valid()) {
            pending_files.insert(file);
        }
    }
    for(auto path: header.owed_units) {
        if(auto unit = fid_of(path); unit.valid()) {
            owed.insert(unit);
        }
    }
    // The header pins the search blob it was saved with: another one
    // describes entries that may have changed since.
    if(auto search_blob = db.read(BlobKind::SearchIndex, "search")) {
        SearchIndex loaded;
        if(loaded.load(std::move(search_blob.buffer)) &&
           loaded.generation() == header.search_generation) {
            search = std::move(loaded);
        }
    }
    return LoadOutcome::Loaded;
}

llvm::SmallVector<Fid> ProjectIndex::stale_units() {
    Bitmap stale;
    scan(file_records, decode_file, [&](llvm::StringRef key, const FileRecord& record) {
        auto file = fid_of(record.path);
        if(!file.valid()) {
            return true;
        }
        bool appeared = record.absent && files.disk.present(file);
        llvm::SmallVector<std::uint64_t> changed;
        for(auto hash: record.consumed) {
            if(files.disk.check(file, hash) != vfs::DiskState::CheckResult::Fresh) {
                changed.push_back(hash);
            }
        }
        if(!appeared && changed.empty()) {
            return true;
        }
        auto* found = read(dependent_records, key, decode_dependents);
        if(!found) {
            return true;
        }
        if(appeared) {
            stale |= found->absent;
        }
        for(auto& keyed: {&found->readers, &found->importers}) {
            for(auto& [hash, units]: *keyed) {
                if(std::ranges::contains(changed, hash)) {
                    stale |= units;
                }
            }
        }
        return true;
    });
    llvm::SmallVector<Fid> units;
    for(auto unit: stale) {
        if(auto tu = fid_of(unit); tu.valid()) {
            units.push_back(tu);
            mark_behind(tu);
        }
    }
    return units;
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
