module clice;

import :index.database;
import :index.project_index;
import :index.project_index.manifest;
import :index.project_index.records;
import :index.shard;
import :index.shard.instantiations;
import :support.logging;

namespace clice::index {

std::string ProjectIndex::portable(llvm::StringRef path) const {
    llvm::SmallString<256> buffer;
    return path::portable(path, project_root, buffer).str();
}

Spelling ProjectIndex::local(llvm::StringRef name) const {
    llvm::SmallString<256> buffer;
    return Spelling::absolute(path::local(name, project_root, buffer));
}

void ProjectIndex::damaged_record(BlobKind kind, llvm::StringRef key) const {
    if(!damage) {
        LOG_ERROR("The index database's {} record {} fails its checks{}",
                  kind,
                  llvm::toHex(key),
                  db->read_only() ? ""
                                  : ": the database is discarded and the project indexed again");
    }
    damage = true;
}

std::string ProjectIndex::file_key(Fid file) const {
    return number_key(path_hash(portable(files.resolve(file))));
}

const FileRecord* ProjectIndex::file_record(Fid file) const {
    return read(file_records, file_key(file), decode_file);
}

FileRecord& ProjectIndex::change_file(Fid file) {
    auto key = file_key(file);
    bool known = read(file_records, key, decode_file) != nullptr;
    auto& record = change(file_records, key, decode_file);
    if(!known) {
        record.path = header.next_path;
        header.next_path += 1;
        path_records.changed[number_key(record.path)] = portable(files.resolve(file));
        fid_paths.try_emplace(file, record.path);
        path_fids.try_emplace(record.path, file);
    }
    return record;
}

const Dependents* ProjectIndex::dependents(Fid file) const {
    return read(dependent_records, file_key(file), decode_dependents);
}

Dependents& ProjectIndex::change_dependents(Fid file) {
    return change(dependent_records, file_key(file), decode_dependents);
}

std::optional<std::uint32_t> ProjectIndex::known_path(Fid file) const {
    if(auto it = fid_paths.find(file); it != fid_paths.end()) {
        return it->second;
    }
    auto* record = file_record(file);
    if(!record) {
        return std::nullopt;
    }
    fid_paths.try_emplace(file, record->path);
    path_fids.try_emplace(record->path, file);
    return record->path;
}

std::uint32_t ProjectIndex::path_id(Fid file) {
    if(auto known = known_path(file)) {
        return *known;
    }
    return change_file(file).path;
}

Fid ProjectIndex::fid_of(std::uint32_t path) const {
    if(auto it = path_fids.find(path); it != path_fids.end()) {
        return it->second;
    }
    auto* name = read(path_records, number_key(path), [](llvm::StringRef bytes) {
        return std::optional(bytes.str());
    });
    if(!name) {
        damaged_record(BlobKind::Path, number_key(path));
        return {};
    }
    auto fid = files.intern(local(*name));
    path_fids.try_emplace(path, fid);
    fid_paths.try_emplace(fid, path);
    return fid;
}

void ProjectIndex::for_each_unit(llvm::function_ref<void(Fid tu)> visit) const {
    scan(file_records, decode_file, [&](llvm::StringRef, const FileRecord& record) {
        if(record.run != 0) {
            if(auto tu = fid_of(record.path); tu.valid()) {
                visit(tu);
            }
        }
        return true;
    });
}

void ProjectIndex::for_each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const {
    auto* found = dependents(file);
    if(!found) {
        return;
    }
    Bitmap units;
    for(auto& set: views::values(found->contributors)) {
        units |= set;
    }
    for(auto unit: units) {
        if(auto tu = fid_of(unit); tu.valid()) {
            visit(tu);
        }
    }
}

void ProjectIndex::for_each_contribution(
    Fid file,
    llvm::function_ref<void(Fid tu, std::uint64_t variant)> visit) const {
    if(auto* found = dependents(file)) {
        for(auto& [variant, units]: found->contributors) {
            for(auto unit: units) {
                if(auto tu = fid_of(unit); tu.valid()) {
                    visit(tu, variant);
                }
            }
        }
    }
}

void ProjectIndex::for_each_absent_lookup(Fid place, llvm::function_ref<void(Fid)> visit) const {
    if(auto* found = dependents(place)) {
        for(auto unit: found->absent) {
            if(auto tu = fid_of(unit); tu.valid()) {
                visit(tu);
            }
        }
    }
}

void ProjectIndex::for_each_importer(Fid input, llvm::function_ref<void(Fid)> visit) const {
    auto* found = dependents(input);
    if(!found) {
        return;
    }
    Bitmap units;
    for(auto& set: views::values(found->importers)) {
        units |= set;
    }
    for(auto unit: units) {
        if(auto tu = fid_of(unit); tu.valid()) {
            visit(tu);
        }
    }
}

std::vector<ResolvedInclude> ProjectIndex::resolved_includes(Fid file) const {
    // Every consumer applies the includes to content the serving shard
    // matches, so a resolution counts only at the shard's content
    // generation — a unit that indexed an older revision would place its
    // lines in text that moved.
    auto* indexed = shard(file);
    auto* found = indexed ? dependents(file) : nullptr;
    if(!found) {
        return {};
    }
    std::vector<ResolvedInclude> includes;
    llvm::DenseSet<std::pair<std::uint32_t, std::uint32_t>> seen;
    for(auto& resolution: found->resolutions) {
        if(resolution.content != indexed->content_hash()) {
            continue;
        }
        for(auto edge: resolution.edges) {
            auto target = fid_of(edge.second);
            if(target.valid() && seen.insert(edge).second) {
                includes.push_back(
                    {.line = edge.first, .target = std::string(files.resolve(target))});
            }
        }
    }
    ranges::stable_sort(includes, [](const ResolvedInclude& lhs, const ResolvedInclude& rhs) {
        return lhs.line < rhs.line;
    });
    return includes;
}

const Shard* ProjectIndex::shard(Fid file) const {
    auto key = file_key(file);
    auto* found = read(shard_records, key, [&](std::unique_ptr<llvm::MemoryBuffer> buffer) {
        auto read = Shard::from_buffer(std::move(buffer));
        if(read.loaded()) {
            read.set_live(live_variants(file));
        }
        return read.loaded() ? std::optional(std::move(read)) : std::nullopt;
    });
    // The file record names the shard it stores.
    if(!found && db && !damage) {
        if(auto* record = file_record(file); record && record->generation != 0) {
            damaged_record(BlobKind::Shard, key);
        }
    }
    return found;
}

void ProjectIndex::for_each_indexed_file(
    llvm::function_ref<void(Fid, std::uint64_t content)> visit) const {
    scan(file_records, decode_file, [&](llvm::StringRef, const FileRecord& record) {
        if(record.generation != 0) {
            if(auto file = fid_of(record.path); file.valid()) {
                visit(file, record.generation);
            }
        }
        return true;
    });
}

std::uint32_t ProjectIndex::shard_count() const {
    std::uint32_t count = 0;
    for_each_indexed_file([&](Fid, std::uint64_t) { count += 1; });
    return count;
}

std::uint64_t ProjectIndex::shard_bytes() const {
    std::uint64_t bytes = 0;
    for_each_indexed_file([&](Fid file, std::uint64_t) {
        if(auto* held = shard(file)) {
            bytes += held->bytes().size();
        }
    });
    return bytes;
}

const InstantiationShard* ProjectIndex::instantiation_shard(Fid file) const {
    return read(instantiation_records,
                file_key(file),
                [&](std::unique_ptr<llvm::MemoryBuffer> buffer) {
                    auto read = InstantiationShard::from_buffer(std::move(buffer));
                    if(read.loaded()) {
                        read.set_live(live_uses(file));
                    }
                    return read.loaded() ? std::optional(std::move(read)) : std::nullopt;
                });
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(Fid file) const {
    llvm::SmallVector<std::uint64_t> variants;
    if(auto* found = dependents(file)) {
        auto keys = views::keys(found->contributors);
        variants.append(keys.begin(), keys.end());
    }
    return variants;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_uses(Fid file) const {
    llvm::SmallVector<std::uint64_t> uses;
    if(auto* found = dependents(file)) {
        auto keys = views::keys(found->uses);
        uses.append(keys.begin(), keys.end());
    }
    return uses;
}

std::optional<Fid> ProjectIndex::find_unit(std::uint64_t hash) const {
    auto* record = read(file_records, number_key(hash), decode_file);
    if(!record || record->run == 0) {
        return std::nullopt;
    }
    auto tu = fid_of(record->path);
    return tu.valid() ? std::optional(tu) : std::nullopt;
}

bool ProjectIndex::current_run(const Producer& producer) const {
    auto* record = read(file_records, number_key(producer.unit), decode_file);
    return record && record->run != 0 && record->run == producer.run;
}

VersionID ProjectIndex::version_of(std::uint32_t id) const {
    if(auto it = id_versions.find(id); it != id_versions.end()) {
        return it->second;
    }
    auto key = number_key(id);
    auto* persisted = read(version_records, key, decode_version);
    if(!persisted) {
        damaged_record(BlobKind::Version, key);
        return {};
    }
    auto file = fid_of(persisted->path);
    if(!file.valid()) {
        return {};
    }
    auto version = files.intern_version(file, persisted->content_hash);
    id_versions.try_emplace(id, version);
    version_ids.try_emplace(version, id);
    return version;
}

void ProjectIndex::retain_version(VersionID version) {
    auto& record = files.version(version);
    auto& versions = change_dependents(record.fid).versions;
    auto it = ranges::lower_bound(versions, record.content_hash, {}, &NamedVersion::content_hash);
    if(it == versions.end() || it->content_hash != record.content_hash) {
        it = versions.insert(it, {.content_hash = record.content_hash, .id = header.next_version});
        header.next_version += 1;
        version_records.changed[number_key(it->id)] =
            PersistedVersion{.path = path_id(record.fid), .content_hash = record.content_hash};
    }
    it->manifests += 1;
    id_versions.try_emplace(it->id, version);
    version_ids.try_emplace(version, it->id);
}

void ProjectIndex::release_version(VersionID version) {
    auto& record = files.version(version);
    auto& versions = change_dependents(record.fid).versions;
    auto it = ranges::lower_bound(versions, record.content_hash, {}, &NamedVersion::content_hash);
    // Only a stored manifest whose dependents record lost the version gets
    // here.
    if(it == versions.end() || it->content_hash != record.content_hash) {
        damaged_record(BlobKind::Dependents, file_key(record.fid));
        return;
    }
    it->manifests -= 1;
    if(it->manifests == 0) {
        version_records.changed[number_key(it->id)] = std::nullopt;
        id_versions.erase(it->id);
        version_ids.erase(version);
        versions.erase(it);
    }
}

void ProjectIndex::replace_shard(Fid file, Shard replacement) {
    auto key = file_key(file);
    update_symbols(file, shard(file), replacement.loaded() ? &replacement : nullptr);
    auto& record = change_file(file);
    record.generation = replacement.loaded() ? replacement.content_hash() : 0;
    record.stored = replacement.variants();
    // A removed shard is held as none: the database keeps its blob until
    // the batch commits, and it must not be read back meanwhile.
    shard_records.changed[key] =
        replacement.loaded() ? std::optional(std::move(replacement)) : std::nullopt;
    search_file_changed(file);
}

void ProjectIndex::replace_instantiations(Fid file, InstantiationShard replacement) {
    update_symbols(file, instantiation_shard(file), replacement.loaded() ? &replacement : nullptr);
    instantiation_records.changed[file_key(file)] =
        replacement.loaded() ? std::optional(std::move(replacement)) : std::nullopt;
}

void ProjectIndex::refresh_live(Fid file) {
    auto key = file_key(file);
    if(auto* held = resident(shard_records, key)) {
        held->set_live(live_variants(file));
    }
    if(auto* held = resident(instantiation_records, key)) {
        held->set_live(live_uses(file));
    }
    compact.insert(file);
    search_file_changed(file);
}

}  // namespace clice::index
