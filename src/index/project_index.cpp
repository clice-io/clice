module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.format;
import :index.instantiation_shard;
import :index.project_index;
import :index.project_records;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
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
    if(!llvm::is_contained(range, value)) {
        range.push_back(value);
    }
}

/// The files a shared local lists, as path ids ascending, two versions of
/// one file once.
std::vector<std::uint32_t> listed_paths(llvm::ArrayRef<VersionID> versions,
                                        llvm::function_ref<std::uint32_t(VersionID)> path_of) {
    std::vector<std::uint32_t> set;
    for(auto version: versions) {
        set.push_back(path_of(version));
    }
    llvm::sort(set);
    set.erase(llvm::unique(set), set.end());
    return set;
}

}  // namespace

ProjectIndex::ProjectIndex(FileTable& files) : files(files) {}

ProjectIndex::~ProjectIndex() = default;
ProjectIndex::ProjectIndex(ProjectIndex&&) noexcept = default;

std::string ProjectIndex::portable(llvm::StringRef path) const {
    llvm::SmallString<256> buffer;
    return path::portable(path, workspace, buffer).str();
}

Spelling ProjectIndex::local(llvm::StringRef name) const {
    llvm::SmallString<256> buffer;
    return Spelling::absolute(path::local(name, workspace, buffer));
}

void ProjectIndex::damaged_record(BlobKind kind, llvm::StringRef key) const {
    LOG_ERROR(
        "The index database's record {} of kind {} fails its checks: the database is discarded "
        "and the project indexed again",
        llvm::toHex(key),
        std::to_underlying(kind));
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

const Symbol* ProjectIndex::symbol(SymbolHash hash) const {
    return read(symbol_records, number_key(hash), decode_symbol);
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

std::optional<SymbolView> ProjectIndex::find_symbol(SymbolHash hash) const {
    auto* found = symbol(hash);
    if(!found) {
        return std::nullopt;
    }
    auto view = found->view();
    if(view.file != no_file) {
        view.file = fid_of(view.file).raw;
    }
    return view;
}

std::uint32_t ProjectIndex::reference_count(SymbolHash hash) const {
    auto* found = symbol(hash);
    return found ? static_cast<std::uint32_t>(found->reference_files.cardinality()) : 0;
}

void ProjectIndex::for_each_reference_file(SymbolHash hash,
                                           llvm::function_ref<void(Fid)> visit) const {
    if(auto* found = symbol(hash)) {
        for(auto path: found->reference_files) {
            if(auto file = fid_of(path); file.valid()) {
                visit(file);
            }
        }
    }
}

void ProjectIndex::for_each_instantiation_file(SymbolHash hash,
                                               llvm::function_ref<void(Fid)> visit) const {
    if(auto* found = symbol(hash)) {
        for(auto path: found->instantiation_files) {
            if(auto file = fid_of(path); file.valid()) {
                visit(file);
            }
        }
    }
}

void ProjectIndex::for_each_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolView&, std::uint32_t)> visit) const {
    scan(symbol_records, decode_symbol, [&](llvm::StringRef key, const Symbol& found) {
        auto hash = key_number<SymbolHash>(key);
        if(!hash || reserved_key(*hash)) {
            damaged_record(BlobKind::Symbol, key);
            return true;
        }
        auto view = found.view();
        if(view.file != no_file) {
            view.file = fid_of(view.file).raw;
        }
        return visit(*hash, view, static_cast<std::uint32_t>(found.reference_files.cardinality()));
    });
}

std::uint32_t ProjectIndex::symbol_count() const {
    return db ? db->count(BlobKind::Symbol) : 0;
}

bool ProjectIndex::indexed(Fid tu) const {
    auto* record = file_record(tu);
    return record && record->run != 0;
}

std::optional<TUManifest> ProjectIndex::manifest(Fid tu) const {
    for(auto* held: {&changed_manifests, &written_manifests}) {
        if(auto it = held->find(tu); it != held->end()) {
            return it->second;
        }
    }
    if(!db || !indexed(tu)) {
        return std::nullopt;
    }
    auto key = file_key(tu);
    auto blob = db->read(BlobKind::TUManifest, key);
    auto decoded = blob ? decode_manifest(blob.buffer->getBuffer()) : std::nullopt;
    if(!decoded) {
        damaged_record(BlobKind::TUManifest, key);
    }
    return decoded;
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
    for(auto& set: llvm::make_second_range(found->contributors)) {
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
    for(auto& set: llvm::make_second_range(found->importers)) {
        units |= set;
    }
    for(auto unit: units) {
        if(auto tu = fid_of(unit); tu.valid()) {
            visit(tu);
        }
    }
}

void ProjectIndex::for_each_file_of_local(SymbolHash hash,
                                          Fid anchor,
                                          llvm::function_ref<void(Fid)> visit) const {
    llvm::SmallVector<llvm::SmallVector<Fid, 4>> sets;
    if(auto* local = read(shared_local_records, number_key(hash), decode_shared_local)) {
        for(auto& listed: llvm::make_first_range(*local)) {
            auto& set = sets.emplace_back();
            for(auto path: listed) {
                if(auto file = fid_of(path); file.valid()) {
                    set.push_back(file);
                }
            }
        }
    }

    llvm::DenseSet<Fid> reached = {anchor};
    visit(anchor);
    llvm::BitVector joined(sets.size());
    for(bool grew = true; grew;) {
        grew = false;
        for(auto [index, set]: llvm::enumerate(sets)) {
            if(joined[index] ||
               llvm::none_of(set, [&](Fid file) { return reached.contains(file); })) {
                continue;
            }
            joined.set(index);
            grew = true;
            for(auto file: set) {
                if(reached.insert(file).second) {
                    visit(file);
                }
            }
        }
    }
}

std::vector<IncludeEdge> ProjectIndex::include_edges(Fid file) const {
    // Every consumer projects the edges onto content the serving shard
    // matches, so a resolution counts only at the shard's content
    // generation — a unit that indexed an older revision would place its
    // lines in text that moved.
    auto* indexed = shard(file);
    auto* found = indexed ? dependents(file) : nullptr;
    if(!found) {
        return {};
    }
    std::vector<IncludeEdge> edges;
    llvm::DenseSet<std::pair<std::uint32_t, std::uint32_t>> seen;
    for(auto& resolution: found->resolutions) {
        if(resolution.content != indexed->content_hash()) {
            continue;
        }
        for(auto edge: resolution.edges) {
            auto target = fid_of(edge.second);
            if(target.valid() && seen.insert(edge).second) {
                edges.push_back({.line = edge.first, .target = std::string(files.resolve(target))});
            }
        }
    }
    llvm::stable_sort(edges, [](const IncludeEdge& lhs, const IncludeEdge& rhs) {
        return lhs.line < rhs.line;
    });
    return edges;
}

const Shard* ProjectIndex::shard(Fid file) const {
    if(auto it = shards.find(file); it != shards.end()) {
        return it->second.loaded() ? &it->second : nullptr;
    }
    // A miss is remembered as an empty entry so the database is asked
    // once per file.
    auto& slot = shards[file];
    if(!db) {
        return nullptr;
    }
    auto key = file_key(file);
    auto blob = db->read(BlobKind::Shard, key);
    if(!blob) {
        return nullptr;
    }
    slot = Shard::from_buffer(std::move(blob.buffer));
    if(!slot.loaded()) {
        damaged_record(BlobKind::Shard, key);
        return nullptr;
    }
    slot.set_live(live_variants(file));
    return &slot;
}

void ProjectIndex::for_each_indexed_file(llvm::function_ref<void(Fid)> visit) const {
    scan(file_records, decode_file, [&](llvm::StringRef, const FileRecord& record) {
        if(record.generation != 0) {
            if(auto file = fid_of(record.path); file.valid()) {
                visit(file);
            }
        }
        return true;
    });
}

std::uint32_t ProjectIndex::shard_count() const {
    std::uint32_t count = 0;
    for_each_indexed_file([&](Fid) { count += 1; });
    return count;
}

std::uint64_t ProjectIndex::shard_bytes() const {
    std::uint64_t bytes = 0;
    for_each_indexed_file([&](Fid file) {
        if(auto it = shards.find(file); it != shards.end() && it->second.loaded()) {
            bytes += it->second.bytes().size();
        } else if(auto blob = db ? db->read(BlobKind::Shard, file_key(file)) : ReadBlob{}) {
            bytes += blob.buffer->getBufferSize();
        }
    });
    return bytes;
}

const InstantiationShard* ProjectIndex::instantiation_shard(Fid file) const {
    if(auto it = instantiations.find(file); it != instantiations.end()) {
        return it->second.loaded() ? &it->second : nullptr;
    }
    auto& slot = instantiations[file];
    if(!db) {
        return nullptr;
    }
    auto key = file_key(file);
    auto blob = db->read(BlobKind::InstantiationShard, key);
    if(!blob) {
        return nullptr;
    }
    slot = InstantiationShard::from_buffer(std::move(blob.buffer));
    if(!slot.loaded()) {
        damaged_record(BlobKind::InstantiationShard, key);
        return nullptr;
    }
    slot.set_live(live_uses(file));
    return &slot;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(Fid file) const {
    llvm::SmallVector<std::uint64_t> variants;
    if(auto* found = dependents(file)) {
        llvm::append_range(variants, llvm::make_first_range(found->contributors));
    }
    return variants;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_uses(Fid file) const {
    llvm::SmallVector<std::uint64_t> uses;
    if(auto* found = dependents(file)) {
        llvm::append_range(uses, llvm::make_first_range(found->uses));
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

Symbol& ProjectIndex::edit(SymbolHash hash) {
    return change(symbol_records, number_key(hash), decode_symbol);
}

void ProjectIndex::merge_symbols(const TUIndex& index,
                                 llvm::ArrayRef<Fid> file_ids_map,
                                 llvm::SmallVectorImpl<SymbolHash>* added) {
    // Units may spell one symbol differently (`X<int>` against
    // `X<signed int>`, a conversion to a typedef): the shortest spelling
    // wins, then the smaller one, so the record reads the same whatever the
    // merge order — among the units that send the symbol.
    //
    // FIXME: what a symbol is can depend on the unit's command — its
    // spelling, SystemHeader under -isystem against -I, Deprecated under a
    // deployment target — and nothing represents that dependence: the
    // record folds whatever units send, and a unit whose every file holding
    // the symbol's entries is a stored variant sends nothing of it
    // (TUIndexOptions::known_variants), so the first unit to store the
    // variant decides. Such facts want one explicit, per-context record
    // instead of a fold.
    auto prefer = [](std::string& current, llvm::StringRef incoming) {
        if(incoming.empty() ||
           (!current.empty() && (incoming.size() > current.size() ||
                                 (incoming.size() == current.size() && incoming >= current)))) {
            return false;
        }
        current = incoming.str();
        return true;
    };
    llvm::SmallVector<std::uint32_t> paths(file_ids_map.size(), no_file);
    auto path_of = [&](std::uint32_t local) {
        if(paths[local] == no_file) {
            paths[local] = path_id(file_ids_map[local]);
        }
        return paths[local];
    };
    index.for_each_symbol(
        [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t> references) {
            if(identity.scope != SymbolScope::External) {
                return true;
            }
            auto* stored = symbol(hash);
            auto target = stored ? *stored : Symbol{};
            bool symbol_changed = !stored;
            bool defines = has_flag(identity.flags, SymbolFlags::HasDefinition);
            if(target.name.empty() && !identity.name.empty()) {
                target.parent = identity.parent;
                target.kind = identity.kind;
                target.token_class.kind = identity.token_class.kind;
                symbol_changed = true;
            } else if(defines && target.token_class.kind != identity.token_class.kind) {
                // A declaration may say `struct` where the definition says
                // `class`: the definition's word stands, whatever merges first.
                target.token_class.kind = identity.token_class.kind;
                symbol_changed = true;
            }
            symbol_changed = prefer(target.name, identity.name) || symbol_changed;
            symbol_changed = prefer(target.args, identity.args) || symbol_changed;
            // An edit to a function, a parameter renamed, keeps its hash: the
            // unit merging last describes it — a unit defining it, when one
            // does, which sees the names of every declaration.
            if(!identity.signature.empty() && target.signature != identity.signature &&
               (defines || !has_flag(target.flags, SymbolFlags::HasDefinition))) {
                target.signature = identity.signature.str();
                symbol_changed = true;
            }
            // A unit that defines the symbol always places it: the record never
            // retracts a unit's earlier report, so an old definition bit must
            // not pin the file after the definition moved. Declarations only
            // fill an empty slot.
            if(identity.file != no_file && (target.file == no_file || defines)) {
                auto file = path_of(identity.file);
                symbol_changed = symbol_changed || target.file != file;
                target.file = file;
            }
            auto flags = target.flags | identity.flags;
            symbol_changed = symbol_changed || flags != target.flags;
            target.flags = flags;
            auto modifiers = target.token_class.modifiers | identity.token_class.modifiers;
            symbol_changed = symbol_changed || modifiers != target.token_class.modifiers;
            target.token_class.modifiers = modifiers;
            for(auto ref: references) {
                target.reference_files.add(path_of(ref));
            }
            if(stored && !symbol_changed && target.reference_files == stored->reference_files) {
                return true;
            }
            edit(hash) = std::move(target);
            if(symbol_changed && added) {
                added->push_back(hash);
            }
            return true;
        });
    index.for_each_instantiation_file([&](SymbolHash hash, llvm::ArrayRef<std::uint32_t> files) {
        auto* stored = symbol(hash);
        if(stored && llvm::all_of(files, [&](std::uint32_t file) {
               return stored->instantiation_files.contains(path_of(file));
           })) {
            return true;
        }
        auto& target = edit(hash);
        for(auto file: files) {
            target.instantiation_files.add(path_of(file));
        }
        return true;
    });
}

std::optional<TUManifest> ProjectIndex::decode_manifest(llvm::StringRef data) const {
    auto persisted = deserialize_manifest(data);
    if(!persisted) {
        return std::nullopt;
    }
    llvm::SmallVector<VersionID> versions;
    for(auto& version: persisted->versions) {
        auto file = fid_of(version.path);
        if(!file.valid()) {
            return std::nullopt;
        }
        versions.push_back(files.intern_version(file, version.content_hash));
    }
    auto& manifest = persisted->manifest;
    for_each_version(manifest, [&](std::uint32_t& id) { id = versions[id].raw; });
    return std::move(manifest);
}

std::vector<std::byte> ProjectIndex::encode_manifest(const TUManifest& manifest) {
    std::vector<PersistedVersion> versions;
    llvm::DenseMap<std::uint32_t, std::uint32_t> local;
    auto persisted = manifest;
    for_each_version(persisted, [&](std::uint32_t& id) {
        auto [it, inserted] = local.try_emplace(id, static_cast<std::uint32_t>(versions.size()));
        if(inserted) {
            auto& record = files.version(VersionID{id});
            versions.push_back({.path = path_id(record.fid), .content_hash = record.content_hash});
        }
        id = it->second;
    });
    return serialize_manifest(persisted, versions);
}

llvm::SmallVector<Fid> ProjectIndex::apply_manifest(Fid tu, TUManifest manifest) {
    // The run is what tells a unit with a manifest (FileRecord::run).
    assert(manifest.built_at != 0 && "a manifest names the run that built it");
    assert(manifest.tu_version.valid() && "a manifest names the unit's version");
    llvm::sort(manifest.absent);
    manifest.absent.erase(llvm::unique(manifest.absent), manifest.absent.end());
    return replace_manifest(tu, std::move(manifest));
}

llvm::SmallVector<Fid> ProjectIndex::remove_manifest(Fid tu) {
    if(!indexed(tu)) {
        return {};
    }
    return replace_manifest(tu, std::nullopt);
}

llvm::SmallVector<Fid> ProjectIndex::replace_manifest(Fid tu, std::optional<TUManifest> manifest) {
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
            auto it = llvm::find_if(resolutions, [&](const auto& resolution) {
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
        for(auto& file: llvm::make_second_range(facts)) {
            llvm::sort(file.read);
            llvm::sort(file.variants);
            llvm::sort(file.imported);
            llvm::sort(file.uses);
            for(auto& edges: llvm::make_second_range(file.resolutions)) {
                llvm::sort(edges);
                edges.erase(llvm::unique(edges), edges.end());
            }
            llvm::sort(file.resolutions);
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
            auto it = llvm::find_if(record.resolutions, [&](const IncludeResolution& resolution) {
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
            auto it = llvm::find_if(record.resolutions, [&](const IncludeResolution& resolution) {
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
            for(auto hash: llvm::make_first_range(*keyed)) {
                file_entry.consumed.push_back(hash);
            }
        }
        llvm::sort(file_entry.consumed);
        file_entry.consumed.erase(llvm::unique(file_entry.consumed), file_entry.consumed.end());
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

    auto locals_of = [&](const TUManifest& of) {
        std::vector<std::pair<SymbolHash, std::vector<std::uint32_t>>> locals;
        for(auto& local: of.shared_locals) {
            locals.emplace_back(local.symbol, listed_paths(local.files, path_of));
        }
        llvm::sort(locals);
        return locals;
    };
    using Locals = std::vector<std::pair<SymbolHash, std::vector<std::uint32_t>>>;
    auto old_locals = old ? locals_of(*old) : Locals();
    auto new_locals = manifest ? locals_of(*manifest) : Locals();
    for(auto& local: old_locals) {
        if(std::ranges::binary_search(new_locals, local)) {
            continue;
        }
        auto& sets = change(shared_local_records, number_key(local.first), decode_shared_local);
        auto it = llvm::lower_bound(sets, local.second, [](const auto& set, const auto& files) {
            return set.first < files;
        });
        if(it != sets.end() && it->first == local.second) {
            it->second -= 1;
            if(it->second == 0) {
                sets.erase(it);
            }
        }
        if(sets.empty()) {
            shared_local_records.changed[number_key(local.first)] = std::nullopt;
        }
    }
    for(auto& local: new_locals) {
        if(std::ranges::binary_search(old_locals, local)) {
            continue;
        }
        auto& sets = change(shared_local_records, number_key(local.first), decode_shared_local);
        auto it = llvm::lower_bound(sets, local.second, [](const auto& set, const auto& files) {
            return set.first < files;
        });
        if(it == sets.end() || it->first != local.second) {
            it = sets.emplace(it, local.second, 0);
        }
        it->second += 1;
    }

    // The unit's previous run walked instantiation keys in the files of its
    // old manifest: the next save drops them.
    if(old) {
        for(auto& use: old->instantiations) {
            compact.insert(files.version(use.file).fid);
        }
    }
    change_file(tu).run = manifest ? manifest->built_at : 0;
    changed_manifests[tu] = std::move(manifest);

    llvm::sort(affected);
    for(auto file: affected) {
        refresh_live(file);
    }
    return affected;
}

void ProjectIndex::refresh_live(Fid file) {
    if(auto it = instantiations.find(file); it != instantiations.end()) {
        it->second.set_live(live_uses(file));
    }
    if(auto it = shards.find(file); it != shards.end()) {
        it->second.set_live(live_variants(file));
    }
    compact.insert(file);
    search_file_changed(file);
}

void ProjectIndex::search_file_changed(Fid file) {
    pending_files.insert(file);
    internals_changed.insert(file);
    if(changed_during_search_build) {
        changed_during_search_build->files.insert(file);
    }
}

std::uint32_t ProjectIndex::pending_shard_writes() const {
    return static_cast<std::uint32_t>(changed_shards.size() + changed_instantiations.size()) +
           writing_shards;
}

bool ProjectIndex::has_unsaved_state() const {
    return !changed_shards.empty() || !changed_instantiations.empty() || !retired_shards.empty() ||
           !retired_instantiations.empty() || !changed_manifests.empty() ||
           !symbol_records.changed.empty() || !file_records.changed.empty() ||
           !dependent_records.changed.empty() || !shared_local_records.changed.empty() ||
           !path_records.changed.empty() || !internals_changed.empty() || !compact.empty() ||
           header_changed || !search_bytes.empty();
}

}  // namespace clice::index
