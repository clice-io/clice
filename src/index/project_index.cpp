module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.format;
import :index.instantiation_shard;
import :index.project_index;
import :index.project_table;
import :index.tu_manifest;
import :semantic.token_class;
import :support.archive;
import :support.logging;

namespace clice::index {

namespace {

/// Upper bound on the persisted FileVersion id space a blob may report:
/// the writer hands ids out from the blob's counter, so a garbage one just
/// below the reserved keys would soon hand one out. The cap is far beyond
/// any lineage a writer accumulates (ids grow only with newly persisted
/// (file, content-hash) pairs), and a lineage that ever drifts there is
/// better compacted by the rebuild the rejection triggers.
constexpr inline std::uint32_t max_persisted_versions = 1u << 24;

/// Drop `unit`'s entry for `file`, and the file once no unit names it. One
/// file's two persisted versions in a manifest reach here twice.
template <typename Table>
void erase_unit(Table& table, Fid file, Fid unit) {
    auto it = table.find(file);
    if(it == table.end()) {
        return;
    }
    it->second.erase(unit);
    if(it->second.empty()) {
        table.erase(it);
    }
}

/// Visit the raw id of every version a manifest names but its failed
/// lookups': what the TU depends on, which its freshness is judged by.
void for_each_dependency(const TUManifest& manifest, llvm::function_ref<void(VersionID)> visit) {
    llvm::DenseSet<VersionID> absent(manifest.absent.begin(), manifest.absent.end());
    for_each_version(manifest, [&](std::uint32_t id) {
        if(!absent.contains(VersionID{id})) {
            visit(VersionID{id});
        }
    });
}

/// The files a shared local lists, ascending, two versions of one file
/// once.
std::vector<Fid> listed_files(const FileTable& files, const SharedLocal& local) {
    std::vector<Fid> set;
    for(auto version: local.files) {
        set.push_back(files.version(version).fid);
    }
    llvm::sort(set);
    set.erase(llvm::unique(set), set.end());
    return set;
}

/// The file table's ids of the path ids of `base` in `ids`; a reader never
/// proved the ids, and one past the path table is skipped.
template <typename Ids>
void for_each_remapped(const ProjectTable& base,
                       const Ids& ids,
                       llvm::function_ref<void(Fid)> visit) {
    for(auto id: ids) {
        if(id < base.remap.size()) {
            visit(base.remap[id]);
        }
    }
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

std::string ProjectIndex::key_of(Fid file) const {
    return blob_key(portable(files.resolve(file)));
}

bool ProjectIndex::bind_table(std::unique_ptr<llvm::MemoryBuffer> blob) {
    if(!blob) {
        return false;
    }
    blob = in_place_buffer(std::move(blob));
    auto root = verify_root<ProjectTableBlob>(blob->getBuffer(), "project table");
    if(!root) {
        return false;
    }
    auto bound = std::make_unique<ProjectTable>();
    if(auto ok = bound->bind(*root); !ok) {
        LOG_DEBUG("Rejecting project table: {}", std::string_view(ok.error()));
        return false;
    }
    // Interning only names the paths: ids left behind by a later reject
    // are inert.
    bound->remap.reserve(bound->paths().size());
    for(std::uint32_t i = 0; i < bound->paths().size(); i += 1) {
        bound->remap.push_back(files.intern(local(llvm::StringRef(bound->paths()[i]))));
    }
    bound->buffer = std::move(blob);
    base = std::move(bound);
    // What was looked up in the previous base names it no more.
    base_unit_files.clear();
    base_unit_index.clear();
    base_units_hashed = false;
    units.clear();
    for(auto tu: llvm::make_first_range(manifests)) {
        units[unit_hash(tu)] = tu;
    }
    overridden_ids = Bitmap();
    for(auto tu: overridden) {
        if(auto id = base->path_id(tu)) {
            overridden_ids.add(*id);
        }
    }
    stale_units.reset();
    return true;
}

std::expected<void, llvm::StringRef> ProjectIndex::adopt_file_versions() {
    if(!base) {
        return {};
    }
    using Blob = ProjectTable::Blob;
    auto& columns = base->columns;
    llvm::ArrayRef<std::uint32_t> version_ids = columns->version_ids;
    auto& version_paths = columns->version_paths;
    llvm::ArrayRef<std::uint64_t> version_hashes = columns->version_hashes;
    llvm::ArrayRef<std::uint32_t> manifest_versions = columns->manifest_versions;
    std::uint32_t next_version_id = columns->next_version_id;
    auto count = version_ids.size();

    // Every value check runs before the first mutation: a blob rejected
    // halfway through would otherwise leave partial state behind — file
    // versions whose corrupt stat stamps feed the freshness fast path —
    // while the caller treats the failed load as "no index on disk".
    for(std::uint32_t i = 0; i < count; i += 1) {
        if(version_paths[i].empty()) {
            return std::unexpected("empty file version path");
        }
    }
    // Every id below becomes a DenseMap or DenseSet key, first in the
    // duplicate checks here and then in the id maps — see reserved_key.
    // The counter is one hand-out away from becoming a key itself, and
    // the writer hands ids out from it, so ids must sit below it — a
    // bound that (with the sentinels at the top of the id space) also
    // keeps every id non-reserved.
    if(reserved_key(next_version_id) || next_version_id > max_persisted_versions) {
        return std::unexpected("file version counter out of range");
    }
    for(auto id: version_ids) {
        if(id >= next_version_id) {
            return std::unexpected("file version id past the counter");
        }
    }
    for(auto version: manifest_versions) {
        if(reserved_key(version)) {
            return std::unexpected("reserved manifest pin");
        }
    }
    // Ids and (path, hash) pairs are both map keys in the writer, so a
    // repeat of either marks a corrupt blob: a repeated id in particular
    // would map to the earlier pair while its record names the later
    // path, attributing contributions to the wrong file.
    llvm::DenseSet<std::uint32_t> blob_version_ids(version_ids.begin(), version_ids.end());
    if(blob_version_ids.size() != count) {
        return std::unexpected("duplicate file version id");
    }
    llvm::DenseSet<std::pair<llvm::StringRef, std::uint64_t>> blob_versions;
    for(std::uint32_t i = 0; i < count; i += 1) {
        if(!blob_versions.insert({llvm::StringRef(version_paths[i]), version_hashes[i]}).second) {
            return std::unexpected("duplicate file version");
        }
    }
    // The writer only pins manifests whose tu_version survived the same save's
    // garbage collection, so an unresolvable pin marks a corrupt blob.
    for(auto version: manifest_versions) {
        if(!blob_version_ids.contains(version)) {
            return std::unexpected("manifest pin names an unknown file version");
        }
    }

    // Two spellings of one file intern to the same version; the ids both
    // stay mapped to it.
    next_persisted_id = next_version_id;
    for(std::uint32_t i = 0; i < count; i += 1) {
        auto path_id = files.intern(local(llvm::StringRef(version_paths[i])));
        auto id = files.intern_version(path_id, version_hashes[i]);
        runtime_ids.try_emplace(version_ids[i], id);
        persisted_ids.try_emplace(id, version_ids[i]);
    }
    return {};
}

bool ProjectIndex::verify_bitmaps() const {
    if(!base) {
        return true;
    }
    auto in_paths = [&](const std::optional<Bitmap>& decoded) {
        return decoded && (decoded->isEmpty() || decoded->maximum() < base->paths().size());
    };
    for(std::uint32_t entry = 0; entry < base->count(); entry += 1) {
        if(!in_paths(base->bitmap(entry)) || !in_paths(base->instantiation_bitmap(entry))) {
            return false;
        }
    }
    return true;
}

std::expected<void, llvm::StringRef> ProjectIndex::load_table(llvm::StringRef data) {
    return load_table(llvm::MemoryBuffer::getMemBufferCopy(data));
}

std::expected<void, llvm::StringRef>
    ProjectIndex::load_table(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!bind_table(std::move(buffer))) {
        return std::unexpected("not a readable project table");
    }
    if(!verify_bitmaps()) {
        base.reset();
        return std::unexpected("reference bitmap does not decode");
    }
    if(auto adopted = adopt_file_versions(); !adopted) {
        base.reset();
        return adopted;
    }
    adopt_base();
    return {};
}

void ProjectIndex::adopt_base() {
    generation = base->columns->generation;
    search_generation = base->columns->search_generation;
    pending_symbols.clear();
    auto& pending = base->columns->search_pending;
    pending_symbols.insert(pending.begin(), pending.end());
    pending_files.clear();
    for(auto file: base->columns->search_pending_files) {
        pending_files.insert(base->remap[file]);
    }
}

const Symbol* ProjectIndex::newer_entry(SymbolHash hash) const {
    if(auto it = changed.find(hash); it != changed.end()) {
        return &it->second;
    }
    if(auto it = written.find(hash); it != written.end()) {
        return &it->second;
    }
    return nullptr;
}

std::optional<SymbolView> ProjectIndex::find_symbol(SymbolHash hash) const {
    if(auto* symbol = newer_entry(hash)) {
        return symbol->view();
    }
    if(base) {
        if(auto entry = base->find(hash)) {
            return base->view(*entry);
        }
    }
    return std::nullopt;
}

std::uint32_t ProjectIndex::reference_count(SymbolHash hash) const {
    if(auto* symbol = newer_entry(hash)) {
        return static_cast<std::uint32_t>(symbol->reference_files.cardinality());
    }
    if(base) {
        if(auto entry = base->find(hash)) {
            return base->reference_counts[*entry];
        }
    }
    return 0;
}

void ProjectIndex::for_each_reference_file(SymbolHash hash,
                                           llvm::function_ref<void(Fid)> visit) const {
    if(auto* symbol = newer_entry(hash)) {
        for(auto id: symbol->reference_files) {
            visit(Fid{id});
        }
        return;
    }
    if(!base) {
        return;
    }
    // A reader never proved the image; one that fails reads as no
    // references rather than crashing the query.
    if(auto entry = base->find(hash)) {
        if(auto decoded = base->bitmap(*entry)) {
            for_each_remapped(*base, *decoded, visit);
        }
    }
}

void ProjectIndex::for_each_instantiation_file(SymbolHash hash,
                                               llvm::function_ref<void(Fid)> visit) const {
    if(auto* symbol = newer_entry(hash)) {
        for(auto id: symbol->instantiation_files) {
            visit(Fid{id});
        }
        return;
    }
    if(!base) {
        return;
    }
    if(auto entry = base->find(hash)) {
        if(auto decoded = base->instantiation_bitmap(*entry)) {
            for_each_remapped(*base, *decoded, visit);
        }
    }
}

void ProjectIndex::for_each_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolView&, std::uint32_t)> visit) const {
    for(auto& [hash, symbol]: changed) {
        if(!visit(hash,
                  symbol.view(),
                  static_cast<std::uint32_t>(symbol.reference_files.cardinality()))) {
            return;
        }
    }
    for(auto& [hash, symbol]: written) {
        if(changed.contains(hash)) {
            continue;
        }
        if(!visit(hash,
                  symbol.view(),
                  static_cast<std::uint32_t>(symbol.reference_files.cardinality()))) {
            return;
        }
    }
    if(!base) {
        return;
    }
    for(std::uint32_t entry = 0; entry < base->count(); entry += 1) {
        auto hash = base->hashes()[entry];
        if(changed.contains(hash) || written.contains(hash)) {
            continue;
        }
        if(!visit(hash, base->view(entry), base->reference_counts[entry])) {
            return;
        }
    }
}

std::uint32_t ProjectIndex::symbol_count() const {
    std::uint32_t count = base ? base->count() : 0;
    for(auto hash: llvm::make_first_range(changed)) {
        if(!written.contains(hash) && !(base && base->find(hash))) {
            count += 1;
        }
    }
    for(auto hash: llvm::make_first_range(written)) {
        if(!(base && base->find(hash))) {
            count += 1;
        }
    }
    return count;
}

Symbol& ProjectIndex::edit(SymbolHash hash) {
    auto [it, inserted] = changed.try_emplace(hash);
    if(!inserted) {
        return it->second;
    }
    auto& symbol = it->second;
    if(auto held = written.find(hash); held != written.end()) {
        symbol = held->second;
        return symbol;
    }
    if(!base) {
        return symbol;
    }
    auto entry = base->find(hash);
    if(!entry) {
        return symbol;
    }
    auto identity = base->view(*entry);
    symbol.name = identity.name.str();
    symbol.args = identity.args.str();
    symbol.signature = identity.signature.str();
    symbol.parent = identity.parent;
    symbol.kind = identity.kind;
    symbol.flags = identity.flags;
    symbol.token_class = identity.token_class;
    symbol.file = identity.file;
    if(auto decoded = base->bitmap(*entry)) {
        for_each_remapped(*base, *decoded, [&](Fid file) { symbol.reference_files.add(file.raw); });
    }
    if(auto decoded = base->instantiation_bitmap(*entry)) {
        for_each_remapped(*base, *decoded, [&](Fid file) {
            symbol.instantiation_files.add(file.raw);
        });
    }
    return symbol;
}

void ProjectIndex::merge_symbols(const TUIndex& index,
                                 llvm::ArrayRef<Fid> file_ids_map,
                                 llvm::SmallVectorImpl<SymbolHash>* added) {
    // Units may spell one symbol differently (`X<int>` against
    // `X<signed int>`, a conversion to a typedef): the shortest spelling
    // wins, then the smaller one, so the table reads the same whatever the
    // merge order — among the units that send the symbol.
    //
    // FIXME: what a symbol is can depend on the unit's command — its
    // spelling, SystemHeader under -isystem against -I, Deprecated under a
    // deployment target — and nothing represents that dependence: the
    // table folds whatever units send, and a unit whose every file holding
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
    index.for_each_symbol(
        [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t> references) {
            if(identity.scope != SymbolScope::External) {
                return true;
            }
            bool known = find_symbol(hash).has_value();
            auto& target = edit(hash);
            bool symbol_changed = !known;
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
            // A unit that defines the symbol always places it: the table never
            // retracts a unit's earlier report, so an old definition bit must
            // not pin the file after the definition moved. Declarations only
            // fill an empty slot.
            if(identity.file != no_file && (target.file == no_file || defines)) {
                auto file = file_ids_map[identity.file].raw;
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
                target.reference_files.add(file_ids_map[ref].raw);
            }
            if(symbol_changed && added) {
                added->push_back(hash);
            }
            return true;
        });
    index.for_each_instantiation_file([&](SymbolHash hash, llvm::ArrayRef<std::uint32_t> files) {
        auto& target = edit(hash);
        for(auto file: files) {
            target.instantiation_files.add(file_ids_map[file].raw);
        }
        return true;
    });
}

std::string ProjectIndex::serialize_table() {
    // The entries changed since the last write join the ones still held
    // aside from a write that was never persisted; the newer entry wins.
    for(auto& [hash, symbol]: changed) {
        written[hash] = std::move(symbol);
    }
    changed.clear();

    ProjectTableBlob blob;
    blob.format_version = index_format_version;
    blob.generation = generation;

    // The base's path table stays as it is, so its entries' bitmap images
    // copy verbatim; files the written entries reference beyond it are
    // appended.
    llvm::DenseMap<Fid, std::uint32_t> id_of;
    if(base) {
        blob.paths.reserve(base->paths().size());
        for(std::uint32_t i = 0; i < base->paths().size(); i += 1) {
            blob.paths.emplace_back(base->paths()[i].str());
            id_of.try_emplace(base->remap[i], i);
        }
    }
    auto path_id = [&](Fid file) {
        auto [it, inserted] =
            id_of.try_emplace(file, static_cast<std::uint32_t>(blob.paths.size()));
        if(inserted) {
            blob.paths.push_back(portable(files.resolve(file)));
        }
        return it->second;
    };

    // A base unit set under the overridden units, with the session's TUs
    // added: the base image as it is when neither changes it.
    auto append_units = [&](std::vector<std::uint8_t>& arena,
                            llvm::ArrayRef<std::uint8_t> image,
                            const Bitmap& added) {
        auto set = image.empty() ? std::optional(Bitmap()) : read_file_set(image);
        if(set && added.isEmpty() && !set->intersect(overridden_ids)) {
            arena.insert(arena.end(), image.begin(), image.end());
            return;
        }
        Bitmap units = set ? *set - overridden_ids : Bitmap();
        units |= added;
        append_file_set(arena, units);
    };
    auto surviving = [&](llvm::ArrayRef<std::uint8_t> image) {
        auto set = read_file_set(image);
        return set && !(*set - overridden_ids).isEmpty();
    };

    // The blob carries only the versions some manifest still references —
    // the persisted form's garbage collection —, each with the units
    // depending on it. The shared table itself is left alone: other
    // consumers may still anchor checks on a version the index dropped, and
    // ids are never reused either way.
    llvm::DenseMap<VersionID, Bitmap> dependents;
    llvm::DenseSet<VersionID> referenced;
    for(auto& [tu, manifest]: manifests) {
        auto tu_id = path_id(tu);
        for_each_version(manifest,
                         [&](std::uint32_t version) { referenced.insert(VersionID{version}); });
        for_each_dependency(manifest, [&](VersionID version) { dependents[version].add(tu_id); });
    }
    llvm::DenseMap<VersionID, std::uint32_t> base_positions;
    if(base) {
        auto& columns = *base->columns;
        for(std::uint32_t position = 0; position < columns.version_ids.size(); position += 1) {
            auto version = runtime_version(columns.version_ids[position]);
            if(version &&
               surviving(slice(columns.version_units, columns.version_unit_ends, position))) {
                referenced.insert(*version);
                base_positions.try_emplace(*version, position);
            }
        }
        // A base unit's failed lookups name versions without bytes the
        // dependency sets leave out.
        for(std::uint32_t k = 0; k < columns.absent_places.size(); k += 1) {
            if(surviving(slice(columns.absent_units, columns.absent_unit_ends, k))) {
                referenced.insert(files.intern_version(base->remap[columns.absent_places[k]], 0));
            }
        }
    }
    llvm::SmallVector<std::pair<std::uint32_t, VersionID>> ids;
    ids.reserve(referenced.size());
    for(auto id: referenced) {
        ids.emplace_back(persisted_id(id), id);
    }
    llvm::sort(ids);
    for(auto [persisted, id]: ids) {
        auto& record = files.version(id);
        blob.version_ids.push_back(persisted);
        blob.version_paths.push_back(portable(files.resolve(record.fid)));
        blob.version_hashes.push_back(record.content_hash);
        auto position = base_positions.find(id);
        auto added = dependents.find(id);
        append_units(blob.version_units,
                     position != base_positions.end() ? slice(base->columns->version_units,
                                                              base->columns->version_unit_ends,
                                                              position->second)
                                                      : llvm::ArrayRef<std::uint8_t>(),
                     added != dependents.end() ? added->second : Bitmap());
        blob.version_unit_ends.push_back(static_cast<std::uint32_t>(blob.version_units.size()));
    }
    blob.next_version_id = next_persisted_id;

    // The units: the base's the session left alone, and the merged ones.
    struct Unit {
        std::uint32_t version;
        std::uint64_t generation;
        std::uint64_t run;
    };

    std::vector<Unit> written_units;
    for(auto tu: base_units()) {
        if(auto unit = base_unit(tu)) {
            auto& columns = *base->columns;
            written_units.push_back({columns.manifest_versions[*unit],
                                     columns.manifest_generations[*unit],
                                     columns.manifest_runs[*unit]});
        }
    }
    for(auto& manifest: llvm::make_second_range(manifests)) {
        written_units.push_back({persisted_ids.find(manifest.tu_version)->second,
                                 manifest.table_generation,
                                 manifest.built_at});
    }
    std::ranges::sort(written_units, {}, &Unit::version);
    for(auto& unit: written_units) {
        blob.manifest_versions.push_back(unit.version);
        blob.manifest_generations.push_back(unit.generation);
        blob.manifest_runs.push_back(unit.run);
    }

    // The table in hash order: base entries copied, written entries encoded.
    struct Entry {
        SymbolHash hash;
        std::uint32_t base_entry;
        const Symbol* symbol;
    };

    std::vector<Entry> entries;
    entries.reserve((base ? base->count() : 0) + written.size());
    llvm::SmallVector<SymbolHash> keys;
    keys.reserve(written.size());
    for(auto hash: llvm::make_first_range(written)) {
        keys.push_back(hash);
    }
    llvm::sort(keys);
    std::uint32_t base_entry = 0;
    std::uint32_t k = 0;
    auto base_count = base ? base->count() : 0;
    while(base_entry < base_count || k < keys.size()) {
        if(k == keys.size() || (base_entry < base_count && base->hashes()[base_entry] < keys[k])) {
            entries.push_back({base->hashes()[base_entry], base_entry, nullptr});
            base_entry += 1;
            continue;
        }
        if(base_entry < base_count && base->hashes()[base_entry] == keys[k]) {
            base_entry += 1;
        }
        entries.push_back({keys[k], 0, &written.find(keys[k])->second});
        k += 1;
    }
    blob.symbol_hashes.reserve(entries.size());
    for(auto& entry: entries) {
        blob.symbol_hashes.push_back(entry.hash);
    }

    std::vector<std::uint32_t> parents;
    std::vector<std::uint32_t> file_ids;
    std::vector<std::uint32_t> reference_counts;
    parents.reserve(entries.size());
    file_ids.reserve(entries.size());
    reference_counts.reserve(entries.size());
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        auto& [hash, base_entry, symbol] = entries[i];
        auto identity = symbol ? symbol->view() : base->view(base_entry);
        blob.symbol_names += identity.name;
        blob.symbol_name_ends.push_back(static_cast<std::uint32_t>(blob.symbol_names.size()));
        if(!identity.args.empty()) {
            blob.symbol_args_entries.push_back(i);
            blob.symbol_args += identity.args;
            blob.symbol_args_ends.push_back(static_cast<std::uint32_t>(blob.symbol_args.size()));
        }
        blob.symbol_signatures += identity.signature;
        blob.symbol_signature_ends.push_back(
            static_cast<std::uint32_t>(blob.symbol_signatures.size()));
        auto parent = identity.parent == 0
                          ? std::nullopt
                          : sorted_find(llvm::ArrayRef(blob.symbol_hashes), identity.parent);
        parents.push_back(parent ? *parent + 1 : 0);
        if(identity.parent != 0 && !parent) {
            blob.symbol_far_parents.push_back(i);
            blob.symbol_far_parent_hashes.push_back(identity.parent);
        }
        blob.symbol_kinds.push_back(identity.kind.value());
        blob.symbol_flags.push_back(static_cast<std::uint16_t>(identity.flags));
        blob.symbol_token_kinds.push_back(static_cast<std::uint8_t>(identity.token_class.kind));
        blob.symbol_token_modifiers.push_back(identity.token_class.modifiers);
        if(!symbol) {
            auto file = base->file(base_entry);
            file_ids.push_back(file == no_file ? 0 : file + 1);
            reference_counts.push_back(base->reference_counts[base_entry]);
            auto image = base->image(base_entry);
            blob.symbol_bitmaps.insert(blob.symbol_bitmaps.end(), image.begin(), image.end());
            blob.symbol_bitmap_ends.push_back(
                static_cast<std::uint32_t>(blob.symbol_bitmaps.size()));
            auto instantiations = base->instantiation_image(base_entry);
            if(!instantiations.empty()) {
                blob.symbol_instantiation_entries.push_back(i);
                blob.symbol_instantiations.insert(blob.symbol_instantiations.end(),
                                                  instantiations.begin(),
                                                  instantiations.end());
                blob.symbol_instantiation_ends.push_back(
                    static_cast<std::uint32_t>(blob.symbol_instantiations.size()));
            }
            continue;
        }
        file_ids.push_back(symbol->file == no_file ? 0 : path_id(Fid{symbol->file}) + 1);
        reference_counts.push_back(
            static_cast<std::uint32_t>(symbol->reference_files.cardinality()));
        Bitmap references;
        for(auto id: symbol->reference_files) {
            references.add(path_id(Fid{id}));
        }
        append_file_set(blob.symbol_bitmaps, references);
        blob.symbol_bitmap_ends.push_back(static_cast<std::uint32_t>(blob.symbol_bitmaps.size()));
        if(!symbol->instantiation_files.isEmpty()) {
            Bitmap instantiations;
            for(auto id: symbol->instantiation_files) {
                instantiations.add(path_id(Fid{id}));
            }
            blob.symbol_instantiation_entries.push_back(i);
            append_file_set(blob.symbol_instantiations, instantiations);
            blob.symbol_instantiation_ends.push_back(
                static_cast<std::uint32_t>(blob.symbol_instantiations.size()));
        }
    }
    write_ids(blob.symbol_parents, parents, entries.size() + 1);

    // What the units' manifests say: per key, the base's sets under the
    // overridden units, the session's TUs added, keys and entries left with
    // no unit dropped.
    struct UnitSet {
        std::uint64_t hash = 0;
        std::uint32_t version = 0;
        llvm::ArrayRef<std::uint8_t> image;
        Bitmap added;
    };

    auto live = [&](const UnitSet& entry) {
        return !entry.added.isEmpty() || (!entry.image.empty() && surviving(entry.image));
    };
    auto add_unit = [&](std::vector<UnitSet>& entries, std::uint64_t hash, std::uint32_t version) {
        auto it = llvm::find_if(entries, [&](const UnitSet& entry) { return entry.hash == hash; });
        if(it == entries.end()) {
            entries.push_back({.hash = hash, .version = version});
            it = std::prev(entries.end());
        }
        return &it->added;
    };

    std::map<std::uint32_t, std::vector<UnitSet>> contributed;
    std::map<std::uint32_t, std::vector<UnitSet>> used;
    std::map<std::uint32_t, UnitSet> places;
    std::map<std::uint32_t, UnitSet> inputs;
    if(base) {
        auto& columns = *base->columns;
        for(std::uint32_t k = 0; k < columns.contributed_files.size(); k += 1) {
            auto& entries = contributed[columns.contributed_files[k]];
            for(auto entry = k == 0 ? 0 : columns.contribution_ends[k - 1];
                entry < columns.contribution_ends[k];
                entry += 1) {
                entries.push_back({
                    .hash = columns.contribution_variants[entry],
                    .version = columns.contribution_versions[entry],
                    .image =
                        slice(columns.contribution_units, columns.contribution_unit_ends, entry),
                });
            }
        }
        for(std::uint32_t k = 0; k < columns.use_files.size(); k += 1) {
            auto& entries = used[columns.use_files[k]];
            for(auto entry = k == 0 ? 0 : columns.use_ends[k - 1]; entry < columns.use_ends[k];
                entry += 1) {
                entries.push_back({
                    .hash = columns.use_hashes[entry],
                    .image = slice(columns.use_units, columns.use_unit_ends, entry),
                });
            }
        }
        for(std::uint32_t k = 0; k < columns.absent_places.size(); k += 1) {
            places[columns.absent_places[k]].image =
                slice(columns.absent_units, columns.absent_unit_ends, k);
        }
        for(std::uint32_t k = 0; k < columns.import_inputs.size(); k += 1) {
            inputs[columns.import_inputs[k]].image =
                slice(columns.import_units, columns.import_unit_ends, k);
        }
    }
    for(auto& [file, tus]: contributions) {
        auto& entries = contributed[path_id(file)];
        for(auto& [tu, contributed_variant]: tus) {
            add_unit(entries,
                     contributed_variant.variant,
                     persisted_id(contributed_variant.version))
                ->add(path_id(tu));
        }
    }
    for(auto& [file, tus]: instantiation_uses) {
        auto& entries = used[path_id(file)];
        for(auto [tu, use]: tus) {
            add_unit(entries, use, 0)->add(path_id(tu));
        }
    }
    for(auto& [place, tus]: absent_lookups) {
        auto& entry = places[path_id(place)];
        for(auto tu: tus) {
            entry.added.add(path_id(tu));
        }
    }
    for(auto& [input, tus]: importers) {
        auto& entry = inputs[path_id(input)];
        for(auto tu: tus) {
            entry.added.add(path_id(tu));
        }
    }

    for(auto& [file, entries]: contributed) {
        std::ranges::sort(entries, {}, &UnitSet::hash);
        auto before = blob.contribution_variants.size();
        for(auto& entry: entries) {
            if(!live(entry)) {
                continue;
            }
            blob.contribution_variants.push_back(entry.hash);
            blob.contribution_versions.push_back(entry.version);
            append_units(blob.contribution_units, entry.image, entry.added);
            blob.contribution_unit_ends.push_back(
                static_cast<std::uint32_t>(blob.contribution_units.size()));
        }
        if(blob.contribution_variants.size() != before) {
            blob.contributed_files.push_back(file);
            blob.contribution_ends.push_back(
                static_cast<std::uint32_t>(blob.contribution_variants.size()));
        }
    }
    for(auto& [file, entries]: used) {
        std::ranges::sort(entries, {}, &UnitSet::hash);
        auto before = blob.use_hashes.size();
        for(auto& entry: entries) {
            if(!live(entry)) {
                continue;
            }
            blob.use_hashes.push_back(entry.hash);
            append_units(blob.use_units, entry.image, entry.added);
            blob.use_unit_ends.push_back(static_cast<std::uint32_t>(blob.use_units.size()));
        }
        if(blob.use_hashes.size() != before) {
            blob.use_files.push_back(file);
            blob.use_ends.push_back(static_cast<std::uint32_t>(blob.use_hashes.size()));
        }
    }
    for(auto& [place, entry]: places) {
        if(live(entry)) {
            blob.absent_places.push_back(place);
            append_units(blob.absent_units, entry.image, entry.added);
            blob.absent_unit_ends.push_back(static_cast<std::uint32_t>(blob.absent_units.size()));
        }
    }
    for(auto& [input, entry]: inputs) {
        if(live(entry)) {
            blob.import_inputs.push_back(input);
            append_units(blob.import_units, entry.image, entry.added);
            blob.import_unit_ends.push_back(static_cast<std::uint32_t>(blob.import_units.size()));
        }
    }

    // A shared local's sets are told apart by their files.
    struct ListedSet {
        Bitmap files;
        UnitSet units;
    };

    std::map<SymbolHash, std::vector<ListedSet>> shared;
    if(base) {
        auto& columns = *base->columns;
        for(std::uint32_t k = 0; k < columns.shared_local_symbols.size(); k += 1) {
            auto& sets = shared[columns.shared_local_symbols[k]];
            for(auto set = k == 0 ? 0 : columns.shared_local_set_ends[k - 1];
                set < columns.shared_local_set_ends[k];
                set += 1) {
                if(auto listed = read_file_set(
                       slice(columns.shared_local_files, columns.shared_local_file_ends, set))) {
                    sets.push_back({
                        .files = std::move(*listed),
                        .units = {.image = slice(columns.shared_local_units,
                                                 columns.shared_local_unit_ends,
                                                 set)},
                    });
                }
            }
        }
    }
    for(auto& [symbol, listed_sets]: shared_local_sets) {
        auto& sets = shared[symbol];
        for(auto& [listed, tus]: listed_sets) {
            Bitmap ids;
            for(auto file: listed) {
                ids.add(path_id(file));
            }
            auto it = llvm::find_if(sets, [&](const ListedSet& set) { return set.files == ids; });
            if(it == sets.end()) {
                sets.push_back({.files = std::move(ids)});
                it = std::prev(sets.end());
            }
            for(auto tu: tus) {
                it->units.added.add(path_id(tu));
            }
        }
    }
    for(auto& [symbol, sets]: shared) {
        auto before = blob.shared_local_file_ends.size();
        for(auto& set: sets) {
            if(!live(set.units)) {
                continue;
            }
            append_file_set(blob.shared_local_files, set.files);
            blob.shared_local_file_ends.push_back(
                static_cast<std::uint32_t>(blob.shared_local_files.size()));
            append_units(blob.shared_local_units, set.units.image, set.units.added);
            blob.shared_local_unit_ends.push_back(
                static_cast<std::uint32_t>(blob.shared_local_units.size()));
        }
        if(blob.shared_local_file_ends.size() != before) {
            blob.shared_local_symbols.push_back(symbol);
            blob.shared_local_set_ends.push_back(
                static_cast<std::uint32_t>(blob.shared_local_file_ends.size()));
        }
    }

    blob.search_generation = search_generation;
    blob.search_pending.assign(pending_symbols.begin(), pending_symbols.end());
    llvm::sort(blob.search_pending);
    for(auto file: pending_files) {
        blob.search_pending_files.push_back(path_id(file));
    }
    llvm::sort(blob.search_pending_files);
    // After every path id is handed out: the width follows the path table.
    write_ids(blob.symbol_files, file_ids, blob.paths.size() + 1);
    write_ids(blob.symbol_reference_counts, reference_counts, blob.paths.size() + 1);

    return archive::encode(blob);
}

void ProjectIndex::rebase(std::unique_ptr<llvm::MemoryBuffer> blob) {
    [[maybe_unused]] bool bound = bind_table(std::move(blob));
    assert(bound && "a freshly serialized project table must bind");
    written.clear();
    // A TU merged again or dropped while the table was being written is a
    // unit of the written table too: the session's word is the newer.
    auto hide = [&](Fid tu) {
        if(base_unit(tu)) {
            hide_base_unit(tu);
        }
    };
    llvm::for_each(llvm::make_first_range(manifests), hide);
    llvm::for_each(dirty_manifests, hide);
}

void ProjectIndex::fold_written() {
    llvm::SmallVector<Fid> folded;
    for(auto tu: overridden) {
        if(!dirty_manifests.contains(tu)) {
            folded.push_back(tu);
        }
    }
    for(auto tu: llvm::make_first_range(manifests)) {
        if(!dirty_manifests.contains(tu) && !overridden.contains(tu)) {
            folded.push_back(tu);
        }
    }
    for(auto tu: folded) {
        overridden.erase(tu);
        auto it = manifests.find(tu);
        if(it == manifests.end()) {
            continue;
        }
        // The table now says what the manifest does: the live sets do not
        // change, only where they are read.
        erase_derived(tu, it->second);
        manifests.erase(it);
    }
}

void ProjectIndex::restore_unwritten() {
    for(auto& [hash, symbol]: written) {
        changed.try_emplace(hash, std::move(symbol));
    }
    written.clear();
}

std::optional<VersionID> ProjectIndex::runtime_version(std::uint32_t persisted) const {
    if(reserved_key(persisted)) {
        return std::nullopt;
    }
    auto it = runtime_ids.find(persisted);
    if(it == runtime_ids.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ProjectIndex::import_manifest(TUManifest& manifest) const {
    bool imported = true;
    for_each_version(manifest, [&](std::uint32_t& id) {
        auto version = imported ? runtime_version(id) : std::nullopt;
        imported = version.has_value();
        if(imported) {
            id = version->raw;
        }
    });
    return imported;
}

TUManifest ProjectIndex::export_manifest(const TUManifest& manifest) {
    auto exported = manifest;
    for_each_version(exported, [&](std::uint32_t& id) { id = persisted_id(VersionID{id}); });
    return exported;
}

std::uint32_t ProjectIndex::persisted_id(VersionID version) {
    auto [it, inserted] = persisted_ids.try_emplace(version, next_persisted_id);
    if(inserted) {
        runtime_ids.try_emplace(next_persisted_id, version);
        next_persisted_id += 1;
    }
    return it->second;
}

llvm::SmallVector<Fid> ProjectIndex::apply_manifest(Fid tu_path_id, TUManifest manifest) {
    llvm::SmallVector<Fid> affected = remove_manifest(tu_path_id);

    for(auto& [version, hash]: manifest.contributions) {
        auto path_id = files.version(version).fid;
        contributions[path_id][tu_path_id] = {.variant = hash, .version = version};
        affected.push_back(path_id);
    }
    // Two spellings of one place, or two persisted versions of it, are one
    // file: remove_manifest erases each place once.
    llvm::sort(manifest.absent);
    manifest.absent.erase(llvm::unique(manifest.absent), manifest.absent.end());
    for(auto version: manifest.absent) {
        absent_lookups[files.version(version).fid].insert(tu_path_id);
    }
    for(auto version: manifest.imports) {
        importers[files.version(version).fid].insert(tu_path_id);
    }
    for(auto& local: manifest.shared_locals) {
        shared_local_sets[local.symbol][listed_files(files, local)].insert(tu_path_id);
    }
    for(auto& use: manifest.instantiations) {
        auto path_id = files.version(use.file).fid;
        instantiation_uses[path_id][tu_path_id] = use.use;
        affected.push_back(path_id);
    }
    units[unit_hash(tu_path_id)] = tu_path_id;
    manifests[tu_path_id] = std::move(manifest);
    // The persisted manifest this one replaces may have been found lost,
    // also by the removal above: it no longer stands for the unit.
    lost_manifests.erase(tu_path_id);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    for(auto file: affected) {
        refresh_live(file);
    }
    return affected;
}

llvm::SmallVector<Fid> ProjectIndex::erase_derived(Fid tu, const TUManifest& manifest) {
    llvm::SmallVector<Fid> affected;
    for(auto version: llvm::make_first_range(manifest.contributions)) {
        auto path_id = files.version(version).fid;
        erase_unit(contributions, path_id, tu);
        affected.push_back(path_id);
    }
    // apply_manifest kept each place once.
    for(auto version: manifest.absent) {
        auto probe_it = absent_lookups.find(files.version(version).fid);
        probe_it->second.erase(tu);
        if(probe_it->second.empty()) {
            absent_lookups.erase(probe_it);
        }
    }
    // Modules built at different times may have read different versions
    // of one file: the first of them already erased it.
    for(auto version: manifest.imports) {
        auto importer_it = importers.find(files.version(version).fid);
        if(importer_it == importers.end()) {
            continue;
        }
        importer_it->second.erase(tu);
        if(importer_it->second.empty()) {
            importers.erase(importer_it);
        }
    }
    for(auto& local: manifest.shared_locals) {
        auto sets = shared_local_sets.find(local.symbol);
        auto set = sets->second.find(listed_files(files, local));
        set->second.erase(tu);
        if(set->second.empty()) {
            sets->second.erase(set);
        }
        if(sets->second.empty()) {
            shared_local_sets.erase(sets);
        }
    }
    for(auto& use: manifest.instantiations) {
        auto path_id = files.version(use.file).fid;
        erase_unit(instantiation_uses, path_id, tu);
        affected.push_back(path_id);
    }
    return affected;
}

llvm::SmallVector<Fid> ProjectIndex::remove_manifest(Fid tu_path_id) {
    auto it = manifests.find(tu_path_id);
    if(it == manifests.end()) {
        auto affected = override_unit(tu_path_id);
        for(auto file: affected) {
            shrunk.insert(file);
            refresh_live(file);
        }
        return affected;
    }

    auto affected = erase_derived(tu_path_id, it->second);
    units.erase(unit_hash(tu_path_id));
    manifests.erase(it);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    for(auto file: affected) {
        shrunk.insert(file);
        refresh_live(file);
    }
    return affected;
}

llvm::ArrayRef<Fid> ProjectIndex::base_units() const {
    if(base && base_unit_files.empty() && base->unit_count() != 0) {
        // Every unit's version resolves: load_table checked the loaded
        // pins, and a written table's ids map back (persisted_id).
        auto& versions = base->columns->manifest_versions;
        base_unit_files.reserve(versions.size());
        for(std::uint32_t unit = 0; unit < versions.size(); unit += 1) {
            auto tu_file = files.version(*runtime_version(versions[unit])).fid;
            base_unit_files.push_back(tu_file);
            base_unit_index.try_emplace(tu_file, unit);
        }
    }
    return base_unit_files;
}

std::optional<std::uint32_t> ProjectIndex::base_unit(Fid tu) const {
    if(!base || overridden.contains(tu)) {
        return std::nullopt;
    }
    base_units();
    auto it = base_unit_index.find(tu);
    return it != base_unit_index.end() ? std::optional(it->second) : std::nullopt;
}

void ProjectIndex::for_each_base_unit(const Bitmap& ids,
                                      llvm::function_ref<void(Fid)> visit) const {
    for(auto id: ids) {
        if(id < base->remap.size() && !overridden_ids.contains(id)) {
            visit(base->remap[id]);
        }
    }
}

std::optional<TUManifest> ProjectIndex::persisted_manifest(Fid tu) const {
    auto unit = base_unit(tu);
    if(!unit || !db || lost_manifests.contains(tu)) {
        return std::nullopt;
    }
    // A blob of another generation than the pin is a failed write the
    // table outran, or one that outran a lost table write.
    auto blob = db->read(BlobKind::TUManifest, key_of(tu));
    auto manifest = blob ? deserialize_manifest(blob.buffer->getBuffer()) : std::nullopt;
    if(!manifest || manifest->tu_version.raw != base->columns->manifest_versions[*unit] ||
       manifest->table_generation != base->columns->manifest_generations[*unit] ||
       !import_manifest(*manifest)) {
        LOG_INFO("Discarding the lost manifest of {}", files.resolve(tu));
        lose(tu, Lost::Manifest);
        return std::nullopt;
    }
    return manifest;
}

void ProjectIndex::hide_base_unit(Fid tu) {
    overridden.insert(tu);
    if(auto id = base->path_id(tu)) {
        overridden_ids.add(*id);
    }
    // A unit the session merged stays found under its own manifest.
    if(!manifests.contains(tu)) {
        units.erase(unit_hash(tu));
    }
}

llvm::SmallVector<Fid> ProjectIndex::override_unit(Fid tu) {
    llvm::SmallVector<Fid> affected;
    if(!base_unit(tu)) {
        return affected;
    }
    auto persisted = persisted_manifest(tu);
    hide_base_unit(tu);
    auto id = base->path_id(tu);
    if(persisted) {
        for(auto version: llvm::make_first_range(persisted->contributions)) {
            affected.push_back(files.version(version).fid);
        }
        for(auto& use: persisted->instantiations) {
            affected.push_back(files.version(use.file).fid);
        }
    } else if(id) {
        // Its manifest is lost: the base's own tables say where it was.
        auto& columns = *base->columns;
        auto& contributed = columns.contributed_files;
        auto& ends = columns.contribution_ends;
        for(std::uint32_t k = 0; k < contributed.size(); k += 1) {
            for(auto entry = k == 0 ? 0 : ends[k - 1]; entry < ends[k]; entry += 1) {
                auto set = read_file_set(
                    slice(columns.contribution_units, columns.contribution_unit_ends, entry));
                if(set && set->contains(*id)) {
                    affected.push_back(base->remap[contributed[k]]);
                }
            }
        }
        for(std::uint32_t k = 0; k < columns.use_files.size(); k += 1) {
            for(auto entry = k == 0 ? 0 : columns.use_ends[k - 1]; entry < columns.use_ends[k];
                entry += 1) {
                auto set = read_file_set(slice(columns.use_units, columns.use_unit_ends, entry));
                if(set && set->contains(*id)) {
                    affected.push_back(base->remap[columns.use_files[k]]);
                }
            }
        }
    }
    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    return affected;
}

void ProjectIndex::refresh_live(Fid file) {
    if(auto it = instantiations.find(file); it != instantiations.end() && it->second.loaded()) {
        it->second.set_live(live_uses(file));
    }
    if(auto it = shards.find(file); it != shards.end() && it->second.loaded()) {
        it->second.set_live(live_variants(file));
    }
    // Read or not, its live variants say which of its internal names live.
    search_file_changed(file);
}

void ProjectIndex::search_file_changed(Fid file) {
    pending_files.insert(file);
    if(changed_during_search_build) {
        changed_during_search_build->files.insert(file);
    }
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(Fid path_id) const {
    llvm::SmallVector<std::uint64_t> hashes;
    for_each_contribution(path_id, [&](Fid, std::uint64_t variant) { hashes.push_back(variant); });
    llvm::sort(hashes);
    hashes.erase(llvm::unique(hashes), hashes.end());
    return hashes;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_uses(Fid path_id) const {
    llvm::SmallVector<std::uint64_t> hashes;
    if(base) {
        for(auto& use: base->uses_of(path_id)) {
            bool named = false;
            for_each_base_unit(use.units, [&](Fid) { named = true; });
            if(named) {
                hashes.push_back(use.use);
            }
        }
    }
    if(auto it = instantiation_uses.find(path_id); it != instantiation_uses.end()) {
        llvm::append_range(hashes, llvm::make_second_range(it->second));
    }
    llvm::sort(hashes);
    hashes.erase(llvm::unique(hashes), hashes.end());
    return hashes;
}

llvm::SmallVector<std::uint64_t, 1> ProjectIndex::generations_of(Fid file) const {
    llvm::SmallVector<std::uint64_t, 1> generations;
    auto add = [&](VersionID version) {
        auto hash = files.version(version).content_hash;
        if(hash != 0 && !llvm::is_contained(generations, hash)) {
            generations.push_back(hash);
        }
    };
    if(base) {
        for(auto& contribution: base->contributions_of(file)) {
            bool contributed = false;
            for_each_base_unit(contribution.units, [&](Fid) { contributed = true; });
            if(auto version = runtime_version(contribution.version); version && contributed) {
                add(*version);
            }
        }
    }
    if(auto it = contributions.find(file); it != contributions.end()) {
        for(auto& contributed: llvm::make_second_range(it->second)) {
            add(contributed.version);
        }
    }
    return generations;
}

std::optional<std::uint64_t> ProjectIndex::variant_generation(Fid file,
                                                              std::uint64_t variant) const {
    std::optional<VersionID> version;
    if(auto it = contributions.find(file); it != contributions.end()) {
        for(auto& contributed: llvm::make_second_range(it->second)) {
            if(contributed.variant == variant) {
                version = contributed.version;
            }
        }
    }
    if(!version && base) {
        for(auto& contribution: base->contributions_of(file)) {
            bool contributed = false;
            for_each_base_unit(contribution.units, [&](Fid) { contributed = true; });
            if(contribution.variant == variant && contributed) {
                version = runtime_version(contribution.version);
            }
        }
    }
    auto hash = version ? files.version(*version).content_hash : 0;
    return hash != 0 ? std::optional(hash) : std::nullopt;
}

std::optional<std::uint64_t> ProjectIndex::shard_generation(Fid file) const {
    if(!shards.contains(file)) {
        if(auto generations = generations_of(file); generations.size() == 1) {
            return generations.front();
        }
    }
    auto* held = shard(file);
    return held ? std::optional(held->content_hash()) : std::nullopt;
}

bool ProjectIndex::has_manifest(Fid tu) const {
    return manifests.contains(tu) || base_unit(tu).has_value();
}

std::optional<TUManifest> ProjectIndex::manifest(Fid tu) const {
    if(auto it = manifests.find(tu); it != manifests.end()) {
        return it->second;
    }
    return persisted_manifest(tu);
}

void ProjectIndex::for_each_unit(llvm::function_ref<void(Fid tu)> visit) const {
    for(auto tu: llvm::make_first_range(manifests)) {
        visit(tu);
    }
    for(auto tu: base_units()) {
        if(!overridden.contains(tu)) {
            visit(tu);
        }
    }
}

std::uint32_t ProjectIndex::unit_count() const {
    std::uint32_t count = 0;
    for_each_unit([&](Fid) { count += 1; });
    return count;
}

std::optional<Fid> ProjectIndex::find_unit(std::uint64_t hash) const {
    if(!base_units_hashed) {
        base_units_hashed = true;
        for(auto tu: base_units()) {
            if(!overridden.contains(tu)) {
                units.try_emplace(unit_hash(tu), tu);
            }
        }
    }
    auto it = units.find(hash);
    return it != units.end() ? std::optional(it->second) : std::nullopt;
}

bool ProjectIndex::current_run(const Producer& producer) const {
    auto tu = find_unit(producer.unit);
    if(!tu) {
        return false;
    }
    if(auto it = manifests.find(*tu); it != manifests.end()) {
        return it->second.built_at == producer.run;
    }
    auto unit = base_unit(*tu);
    return unit && base->columns->manifest_runs[*unit] == producer.run;
}

void ProjectIndex::for_each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const {
    for_each_contribution(file, [&](Fid tu, std::uint64_t) { visit(tu); });
}

void ProjectIndex::for_each_contribution(
    Fid file,
    llvm::function_ref<void(Fid tu, std::uint64_t variant)> visit) const {
    if(base) {
        for(auto& contribution: base->contributions_of(file)) {
            for_each_base_unit(contribution.units,
                               [&](Fid tu) { visit(tu, contribution.variant); });
        }
    }
    if(auto it = contributions.find(file); it != contributions.end()) {
        for(auto& [tu, contributed]: it->second) {
            visit(tu, contributed.variant);
        }
    }
}

void ProjectIndex::for_each_indexed_file(llvm::function_ref<void(Fid)> visit) const {
    llvm::DenseSet<Fid> seen;
    if(base) {
        for(auto id: base->columns->contributed_files) {
            auto file = base->remap[id];
            bool contributed = false;
            for(auto& contribution: base->contributions_of(file)) {
                for_each_base_unit(contribution.units, [&](Fid) { contributed = true; });
            }
            if(contributed && seen.insert(file).second) {
                visit(file);
            }
        }
    }
    for(auto file: llvm::make_first_range(contributions)) {
        if(seen.insert(file).second) {
            visit(file);
        }
    }
}

void ProjectIndex::for_each_absent_lookup(Fid place, llvm::function_ref<void(Fid)> visit) const {
    if(base) {
        if(auto tus = base->absent_units(place)) {
            for_each_base_unit(*tus, visit);
        }
    }
    if(auto it = absent_lookups.find(place); it != absent_lookups.end()) {
        llvm::for_each(it->second, visit);
    }
}

void ProjectIndex::for_each_importer(Fid input, llvm::function_ref<void(Fid)> visit) const {
    if(base) {
        if(auto tus = base->import_units(input)) {
            for_each_base_unit(*tus, visit);
        }
    }
    if(auto it = importers.find(input); it != importers.end()) {
        llvm::for_each(it->second, visit);
    }
}

void ProjectIndex::for_each_file_of_local(SymbolHash hash,
                                          Fid anchor,
                                          llvm::function_ref<void(Fid)> visit) const {
    llvm::SmallVector<llvm::SmallVector<Fid, 4>> sets;
    if(base) {
        for(auto& listed: base->shared_local_sets_of(hash)) {
            bool named = false;
            for_each_base_unit(listed.units, [&](Fid) { named = true; });
            if(!named) {
                continue;
            }
            auto& set = sets.emplace_back();
            for(auto id: listed.files) {
                if(id < base->remap.size()) {
                    set.push_back(base->remap[id]);
                }
            }
        }
    }
    if(auto it = shared_local_sets.find(hash); it != shared_local_sets.end()) {
        for(auto& set: llvm::make_first_range(it->second)) {
            sets.emplace_back(set.begin(), set.end());
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
    // matches, so a manifest contributes only where the version it entered
    // for this document carries that same content generation — a TU that
    // indexed an older revision would place its lines in text that moved.
    auto* indexed = shard(file);
    if(!indexed) {
        return {};
    }
    auto generation = indexed->content_hash();

    auto is_document = [&](VersionID version) {
        auto& entered = files.version(version);
        return entered.fid == file && entered.content_hash == generation;
    };

    // A directive line of the document is a node whose parent node entered
    // this file: the TU root (parent == no_node) when the document is the TU
    // itself, or any node of the document's own file version otherwise
    // (directives inside included headers hang off the node of the file
    // that contains them).
    std::vector<IncludeEdge> edges;
    auto append = [&](const TUManifest& manifest) {
        bool root_is_document = is_document(manifest.tu_version);
        llvm::SmallVector<bool> document_nodes(manifest.nodes.size());
        for(auto [i, node]: llvm::enumerate(manifest.nodes)) {
            document_nodes[i] = is_document(VersionID{node.file});
        }
        for(const auto& node: manifest.nodes) {
            if(node.parent == no_node ? !root_is_document : !document_nodes[node.parent]) {
                continue;
            }
            edges.push_back({
                .line = node.line,
                .target = std::string(files.resolve(files.version(VersionID{node.file}).fid)),
            });
        }
    };

    // The document's own manifest is its own context and answers alone; a
    // header reached only through source TUs has none, and its directives
    // live in the contributing TUs' manifests instead: one TU per variant,
    // since the TUs sharing one parsed the document alike. The generation
    // gate above dedups divergent revisions; agreeing TUs collapse in the
    // projection's dedup.
    if(auto own = manifest(file)) {
        append(*own);
        return edges;
    }
    llvm::SmallDenseMap<std::uint64_t, Fid, 2> readers;
    for_each_contribution(file,
                          [&](Fid tu, std::uint64_t variant) { readers.try_emplace(variant, tu); });
    for(auto tu: llvm::make_second_range(readers)) {
        if(auto contributing = manifest(tu)) {
            append(*contributing);
        }
    }
    return edges;
}

std::unique_ptr<llvm::MemoryBuffer> ProjectIndex::read_blob(BlobKind kind, Fid file) const {
    auto blob = db->read(kind, key_of(file));
    if(!blob) {
        return nullptr;
    }
    if(!borrow && blob.generation != 0) {
        return llvm::MemoryBuffer::getMemBufferCopy(blob.buffer->getBuffer());
    }
    return std::move(blob.buffer);
}

void ProjectIndex::lose(Fid file, Lost kind) const {
    bool first = lost_shards.empty() && lost_instantiations.empty() && lost_manifests.empty();
    switch(kind) {
        case Lost::Shard: lost_shards.insert(file); break;
        case Lost::Uses: lost_instantiations.insert(file); break;
        case Lost::Manifest: lost_manifests.insert(file); break;
    }
    if(first) {
        on_data_lost.emit();
    }
}

Shard ProjectIndex::fetch_shard(Fid file) const {
    auto live = live_variants(file);
    if(live.empty() || !db) {
        return {};
    }
    auto buffer = read_blob(BlobKind::Shard, file);
    auto shard = buffer ? Shard::from_buffer(std::move(buffer)) : Shard();
    // A blob can verify yet miss a contributed variant, or carry another
    // content generation than the contributions pin (crash or failed write
    // left a manifest newer than its shard): set_live would drop missing
    // variants silently and stale content misplaces every position, so
    // both are as lost as an unreadable blob. The generation check catches
    // a stale shard the variant check alone cannot, when an edit past every
    // indexed entry left the variant hash identical.
    auto generations = generations_of(file);
    bool servable =
        shard.loaded() &&
        llvm::all_of(generations,
                     [&](std::uint64_t hash) { return hash == shard.content_hash(); }) &&
        llvm::all_of(live, [&](std::uint64_t hash) { return shard.has_variant(hash); });
    if(!servable) {
        LOG_INFO("Discarding unservable shard for {}", files.resolve(file));
        lose(file, Lost::Shard);
        return {};
    }
    shard.set_live(live);
    return shard;
}

InstantiationShard ProjectIndex::fetch_instantiations(Fid file, std::uint64_t generation) const {
    auto uses = live_uses(file);
    if(!db) {
        return {};
    }
    auto buffer = read_blob(BlobKind::InstantiationShard, file);
    auto loaded =
        buffer ? InstantiationShard::from_buffer(std::move(buffer)) : InstantiationShard();
    // A stored blob names the run behind each key; a worker's names none.
    auto keys = loaded.keys();
    if(loaded.loaded() && (loaded.content_hash() != generation ||
                           (!keys.empty() && !loaded.find_key(keys.front())->producer))) {
        LOG_INFO("Discarding unservable instantiations for {}", files.resolve(file));
        loaded = {};
    }
    // A use some unit names that the file no longer stores lost what that
    // unit's instantiations resolved there: it reindexes, keeping its index
    // meanwhile.
    if(llvm::any_of(uses,
                    [&](std::uint64_t use) { return !loaded.loaded() || !loaded.has_use(use); })) {
        lose(file, Lost::Uses);
    }
    if(loaded.loaded()) {
        loaded.set_live(uses);
    }
    return loaded;
}

const Shard* ProjectIndex::shard(Fid file) const {
    auto it = shards.find(file);
    if(it == shards.end()) {
        it = shards.emplace(file, fetch_shard(file)).first;
    }
    return it->second.loaded() ? &it->second : nullptr;
}

void ProjectIndex::for_each_shard(llvm::function_ref<void(Fid, const Shard&)> visit) const {
    for(auto& [file, shard]: shards) {
        if(shard.loaded()) {
            visit(file, shard);
        }
    }
}

std::uint32_t ProjectIndex::shard_count() const {
    return static_cast<std::uint32_t>(
        llvm::count_if(llvm::make_second_range(shards),
                       [](const Shard& shard) { return shard.loaded(); }));
}

const InstantiationShard* ProjectIndex::instantiation_shard(Fid file) const {
    auto it = instantiations.find(file);
    if(it == instantiations.end()) {
        auto generation = shard_generation(file);
        it = instantiations
                 .emplace(file,
                          generation ? fetch_instantiations(file, *generation)
                                     : InstantiationShard())
                 .first;
    }
    return it->second.loaded() ? &it->second : nullptr;
}

void ProjectIndex::for_each_instantiation_shard(
    llvm::function_ref<void(Fid, const InstantiationShard&)> visit) const {
    for(auto& [file, instantiation_shard]: instantiations) {
        if(instantiation_shard.loaded()) {
            visit(file, instantiation_shard);
        }
    }
}

void ProjectIndex::read_all() const {
    for_each_indexed_file([&](Fid file) {
        shard(file);
        instantiation_shard(file);
    });
    for_each_unit([&](Fid tu) { manifest(tu); });
}

llvm::SetVector<Fid> ProjectIndex::lost_owners() const {
    // A unit merged since the loss holds its own data: a merged manifest
    // replaces the lost one, and a merge that started a lost blob over holds
    // the variant it contributed.
    llvm::SetVector<Fid> owners;
    for(auto tu: lost_manifests) {
        if(base_unit(tu)) {
            owners.insert(tu);
        }
    }
    for(auto file: lost_shards) {
        auto it = shards.find(file);
        auto* current = it != shards.end() && it->second.loaded() ? &it->second : nullptr;
        for_each_contribution(file, [&](Fid tu, std::uint64_t variant) {
            if(!current || !current->has_variant(variant)) {
                owners.insert(tu);
            }
        });
    }
    return owners;
}

llvm::SmallVector<Fid> ProjectIndex::lost_units() const {
    auto lost = lost_owners();
    for(auto file: lost_instantiations) {
        auto it = instantiations.find(file);
        auto* current = it != instantiations.end() && it->second.loaded() ? &it->second : nullptr;
        auto add = [&](Fid tu, std::uint64_t use) {
            if(!current || !current->has_use(use)) {
                lost.insert(tu);
            }
        };
        if(base) {
            for(auto& use: base->uses_of(file)) {
                for_each_base_unit(use.units, [&](Fid tu) { add(tu, use.use); });
            }
        }
        if(auto uses = instantiation_uses.find(file); uses != instantiation_uses.end()) {
            for(auto [tu, use]: uses->second) {
                add(tu, use);
            }
        }
    }
    return llvm::SmallVector<Fid>(lost.begin(), lost.end());
}

IndexReport ProjectIndex::take_lost() {
    IndexReport report;
    for(auto tu: lost_units()) {
        report.add_reindex(tu);
    }
    // An owner's variants are unservable: it loses its manifest, which
    // retires its contributions to every file it touched.
    for(auto tu: lost_owners()) {
        drop(tu, report);
    }
    lost_shards.clear();
    lost_instantiations.clear();
    lost_manifests.clear();
    return report;
}

bool ProjectIndex::bind_search(std::unique_ptr<llvm::MemoryBuffer> blob) {
    SearchIndex built;
    if(!built.load(std::move(blob)) || built.generation() != search_generation) {
        return false;
    }
    search = std::move(built);
    return true;
}

}  // namespace clice::index
