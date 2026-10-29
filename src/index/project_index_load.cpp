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
    // A segment that does not load leaves the index missing: searches scan
    // the records until a full build, and the header stops listing the runs,
    // which no longer receive the changes saves record.
    std::uint32_t first = 0;
    for(auto end: header.search_run_ends) {
        std::vector<std::unique_ptr<llvm::MemoryBuffer>> run;
        for(auto id: llvm::ArrayRef(header.search_segments).slice(first, end - first)) {
            if(auto segment = db.read(BlobKind::SearchSegment, number_key(id))) {
                run.push_back(std::move(segment.buffer));
            } else {
                break;
            }
        }
        if(run.size() != end - first || !search.replace(0, std::move(run))) {
            LOG_INFO("The search index does not load; it is built again");
            search.clear();
            drop_search_runs(static_cast<std::uint32_t>(header.search_run_ends.size()));
            break;
        }
        first = end;
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
                if(ranges::contains(changed, hash)) {
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
