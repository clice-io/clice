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
    // are inert. Two paths naming one file — a symlink made since the write
    // — would key the units' records twice.
    bound->remap.reserve(bound->paths().size());
    for(std::uint32_t i = 0; i < bound->paths().size(); i += 1) {
        auto file = files.intern(local(llvm::StringRef(bound->paths()[i])));
        if(!bound->path_index.try_emplace(file, i).second) {
            LOG_DEBUG("Rejecting project table: two paths name {}", files.resolve(file));
            return false;
        }
        bound->remap.push_back(file);
    }
    bound->buffer = std::move(blob);
    base = std::move(bound);
    hidden = Bitmap();
    base_units_hashed = false;
    stale_units.reset();
    return true;
}

std::expected<void, llvm::StringRef> ProjectIndex::adopt_file_versions() {
    if(!base) {
        return {};
    }
    auto& columns = *base->columns;
    auto& versions = columns.versions;
    std::uint32_t next_version_id = columns.next_version_id;

    // Every value check runs before the first mutation: a blob rejected
    // halfway through would otherwise leave partial state behind — file
    // versions whose corrupt stat stamps feed the freshness fast path —
    // while the caller treats the failed load as "no index on disk".
    //
    // Every id below becomes a DenseMap or DenseSet key, first in the
    // duplicate checks here and then in the id maps — see reserved_key.
    // The counter is one hand-out away from becoming a key itself, and
    // the writer hands ids out from it, so ids must sit below it — a
    // bound that (with the sentinels at the top of the id space) also
    // keeps every id non-reserved.
    if(reserved_key(next_version_id) || next_version_id > max_persisted_versions) {
        return std::unexpected("file version counter out of range");
    }
    // Ids and versions are both map keys in the writer, so a repeat of
    // either marks a corrupt blob: a repeated id would map to the earlier
    // version while its record names the later file, attributing
    // contributions to the wrong file, and a repeated version would keep
    // one of its ids, leaving the records naming the other unresolved.
    llvm::DenseSet<std::uint32_t> blob_version_ids;
    llvm::DenseSet<std::pair<std::uint32_t, std::uint64_t>> blob_versions;
    for(auto& version: versions) {
        if(version.file >= base->remap.size()) {
            return std::unexpected("file version outside the path table");
        }
        if(version.id >= next_version_id) {
            return std::unexpected("file version id past the counter");
        }
        if(!blob_version_ids.insert(version.id).second) {
            return std::unexpected("duplicate file version id");
        }
        if(!blob_versions.insert({version.file, version.hash}).second) {
            return std::unexpected("duplicate file version");
        }
    }
    // The writer only pins manifests whose tu_version survived the same save's
    // garbage collection, so an unresolvable pin marks a corrupt blob.
    for(auto& unit: columns.units) {
        if(!blob_version_ids.contains(unit.version)) {
            return std::unexpected("manifest pin names an unknown file version");
        }
    }

    next_persisted_id = next_version_id;
    for(auto& version: versions) {
        auto id = files.intern_version(base->remap[version.file], version.hash);
        runtime_ids.try_emplace(version.id, id);
        persisted_ids.try_emplace(id, version.id);
    }
    return {};
}

std::expected<void, llvm::StringRef> ProjectIndex::load_table(llvm::StringRef data) {
    return load_table(llvm::MemoryBuffer::getMemBufferCopy(data));
}

std::expected<void, llvm::StringRef>
    ProjectIndex::load_table(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!bind_table(std::move(buffer))) {
        return std::unexpected("not a readable project table");
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
    // An image that does not decode reads as no references.
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
        // A hash is a table key downstream, which reserves two values.
        auto hash = base->hashes()[entry];
        if(reserved_key(hash) || changed.contains(hash) || written.contains(hash)) {
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

    // A base unit set under the hidden units with the session's units added
    // — the base image as it is when neither changes it —; none when no
    // unit is left.
    auto unit_set = [&](llvm::ArrayRef<std::uint8_t> image,
                        const Bitmap& added) -> std::optional<std::vector<std::uint8_t>> {
        auto set = read_file_set(image).value_or(Bitmap());
        if(added.isEmpty() && !set.intersect(hidden)) {
            return set.isEmpty()
                       ? std::nullopt
                       : std::optional(std::vector<std::uint8_t>(image.begin(), image.end()));
        }
        // The decoded set is a view of the image: the result is a new one.
        auto units = set - hidden;
        units |= added;
        if(units.isEmpty()) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes;
        append_file_set(bytes, units);
        return bytes;
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
    llvm::DenseMap<VersionID, std::vector<std::uint8_t>> version_units;
    if(base) {
        for(auto& version: base->columns->versions) {
            auto id = runtime_version(version.id);
            if(!id) {
                continue;
            }
            if(auto units = unit_set(version.units, dependents.lookup(*id))) {
                referenced.insert(*id);
                version_units[*id] = std::move(*units);
            }
            dependents.erase(*id);
        }
        // A base unit's failed lookups name versions without bytes the
        // dependency sets leave out.
        for(auto& place: base->columns->absent) {
            if(unit_set(place.units, {})) {
                referenced.insert(files.intern_version(base->remap[place.key], 0));
            }
        }
    }
    for(auto& [id, added]: dependents) {
        version_units[id] = *unit_set({}, added);
    }
    llvm::SmallVector<std::pair<std::uint32_t, VersionID>> ids;
    ids.reserve(referenced.size());
    for(auto id: referenced) {
        ids.emplace_back(persisted_id(id), id);
    }
    llvm::sort(ids);
    for(auto [persisted, id]: ids) {
        auto& record = files.version(id);
        auto units = version_units.find(id);
        blob.versions.push_back({
            .id = persisted,
            .file = path_id(record.fid),
            .hash = record.content_hash,
            .units = units != version_units.end() ? std::move(units->second)
                                                  : std::vector<std::uint8_t>(),
        });
    }
    blob.next_version_id = next_persisted_id;

    // The units: the base's the session left alone, and the merged ones.
    if(base) {
        for(auto& unit: base->columns->units) {
            if(!hidden.contains(unit.file)) {
                blob.units.push_back(unit);
            }
        }
    }
    for(auto& [tu, manifest]: manifests) {
        blob.units.push_back({
            .file = path_id(tu),
            .version = persisted_ids.find(manifest.tu_version)->second,
            .generation = manifest.table_generation,
            .run = manifest.built_at,
        });
    }
    std::ranges::sort(blob.units, {}, &PersistedUnit::file);

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

    // What the units' manifests say, per key: the base's set under the
    // hidden units with the session's units added; a key left with no unit
    // is dropped.
    struct Said {
        llvm::ArrayRef<std::uint8_t> image;
        Bitmap added;
        std::uint32_t version = 0;
    };

    std::map<std::uint32_t, std::map<std::uint64_t, Said>> contributed;
    std::map<std::uint32_t, std::map<std::uint64_t, Said>> used;
    std::map<std::uint64_t, Said> places;
    std::map<std::uint64_t, Said> inputs;
    std::map<SymbolHash, std::vector<std::pair<Bitmap, Said>>> shared;
    if(base) {
        auto& columns = *base->columns;
        for(auto& file: columns.contributions) {
            for(auto& entry: file.variants) {
                contributed[file.file][entry.variant] = {.image = entry.units,
                                                         .version = entry.version};
            }
        }
        for(auto& file: columns.uses) {
            for(auto& entry: file.uses) {
                used[file.file][entry.key].image = entry.units;
            }
        }
        for(auto& place: columns.absent) {
            places[place.key].image = place.units;
        }
        for(auto& input: columns.imports) {
            inputs[input.key].image = input.units;
        }
        for(auto& entry: columns.shared_locals) {
            for(auto& set: entry.sets) {
                if(auto listed = read_file_set(set.files)) {
                    shared[entry.symbol].emplace_back(std::move(*listed), Said{.image = set.units});
                }
            }
        }
    }
    for(auto& [file, tus]: contributions) {
        auto& entries = contributed[path_id(file)];
        for(auto& [tu, contribution]: tus) {
            auto& said = entries[contribution.variant];
            said.version = persisted_id(contribution.version);
            said.added.add(path_id(tu));
        }
    }
    for(auto& [file, tus]: instantiation_uses) {
        auto& entries = used[path_id(file)];
        for(auto [tu, use]: tus) {
            entries[use].added.add(path_id(tu));
        }
    }
    for(auto& [place, tus]: absent_lookups) {
        for(auto tu: tus) {
            places[path_id(place)].added.add(path_id(tu));
        }
    }
    for(auto& [input, tus]: importers) {
        for(auto tu: tus) {
            inputs[path_id(input)].added.add(path_id(tu));
        }
    }
    for(auto& [symbol, listed_sets]: shared_local_sets) {
        auto& sets = shared[symbol];
        for(auto& [listed, tus]: listed_sets) {
            Bitmap ids;
            for(auto file: listed) {
                ids.add(path_id(file));
            }
            auto it = llvm::find_if(sets, [&](const auto& set) { return set.first == ids; });
            if(it == sets.end()) {
                sets.emplace_back(std::move(ids), Said{});
                it = std::prev(sets.end());
            }
            for(auto tu: tus) {
                it->second.added.add(path_id(tu));
            }
        }
    }

    for(auto& [file, entries]: contributed) {
        ContributedFile record{.file = file};
        for(auto& [variant, said]: entries) {
            if(auto units = unit_set(said.image, said.added)) {
                record.variants.push_back(
                    {.variant = variant, .version = said.version, .units = std::move(*units)});
            }
        }
        if(!record.variants.empty()) {
            blob.contributions.push_back(std::move(record));
        }
    }
    for(auto& [file, entries]: used) {
        UsedFile record{.file = file};
        for(auto& [use, said]: entries) {
            if(auto units = unit_set(said.image, said.added)) {
                record.uses.push_back({.key = use, .units = std::move(*units)});
            }
        }
        if(!record.uses.empty()) {
            blob.uses.push_back(std::move(record));
        }
    }
    auto keyed = [&](const std::map<std::uint64_t, Said>& saids, std::vector<UnitSet>& records) {
        for(auto& [key, said]: saids) {
            if(auto units = unit_set(said.image, said.added)) {
                records.push_back({.key = key, .units = std::move(*units)});
            }
        }
    };
    keyed(places, blob.absent);
    keyed(inputs, blob.imports);
    for(auto& [symbol, sets]: shared) {
        SharedLocalEntry record{.symbol = symbol};
        for(auto& [listed, said]: sets) {
            if(auto units = unit_set(said.image, said.added)) {
                SharedLocalSet set{.units = std::move(*units)};
                append_file_set(set.files, listed);
                record.sets.push_back(std::move(set));
            }
        }
        if(!record.sets.empty()) {
            blob.shared_locals.push_back(std::move(record));
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
    // The written table says what the manifests no merge dirtied since say:
    // the live sets do not change, only where they are read.
    llvm::SmallVector<Fid> written_units;
    for(auto tu: llvm::make_first_range(manifests)) {
        if(!dirty_manifests.contains(tu)) {
            written_units.push_back(tu);
        }
    }
    for(auto tu: written_units) {
        auto it = manifests.find(tu);
        erase_derived(tu, it->second);
        manifests.erase(it);
    }
    [[maybe_unused]] bool bound = bind_table(std::move(blob));
    assert(bound && "a freshly serialized project table must bind");
    written.clear();
    // A TU merged again or dropped while the table was being written is a
    // unit of the written table too: the session's word is the newer.
    llvm::for_each(dirty_manifests, [&](Fid tu) { hide_base_unit(tu); });
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
    llvm::SmallVector<Fid> affected;
    if(auto it = manifests.find(tu_path_id); it != manifests.end()) {
        affected = erase_derived(tu_path_id, it->second);
        manifests.erase(it);
    } else {
        affected = override_unit(tu_path_id);
    }
    // Merged or dropped since, the unit holds its own data or none.
    lost_indexes.erase(tu_path_id);
    lost_uses.erase(tu_path_id);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    for(auto file: affected) {
        shrunk.insert(file);
        refresh_live(file);
    }
    return affected;
}

const PersistedUnit* ProjectIndex::base_unit(Fid tu) const {
    auto* unit = base ? base->unit(tu) : nullptr;
    return unit && !hidden.contains(unit->file) ? unit : nullptr;
}

void ProjectIndex::for_each_base_unit(llvm::ArrayRef<std::uint8_t> units,
                                      llvm::function_ref<void(Fid)> visit) const {
    // A set that does not decode names no unit.
    for(auto id: read_file_set(units).value_or(Bitmap())) {
        if(id < base->remap.size() && !hidden.contains(id)) {
            visit(base->remap[id]);
        }
    }
}

std::optional<TUManifest> ProjectIndex::persisted_manifest(Fid tu) const {
    auto* unit = base_unit(tu);
    if(!unit || !db) {
        return std::nullopt;
    }
    // A blob of another generation than the pin is a failed write the
    // table outran, or one that outran a lost table write.
    auto blob = db->read(BlobKind::TUManifest, key_of(tu));
    auto manifest = blob ? deserialize_manifest(blob.buffer->getBuffer()) : std::nullopt;
    if(!manifest || manifest->tu_version.raw != unit->version ||
       manifest->table_generation != unit->generation || !import_manifest(*manifest)) {
        LOG_INFO("Discarding the lost manifest of {}", files.resolve(tu));
        lose(tu, Lost::Index);
        return std::nullopt;
    }
    return manifest;
}

void ProjectIndex::hide_base_unit(Fid tu) {
    if(auto* unit = base_unit(tu)) {
        hidden.add(unit->file);
    }
}

llvm::SmallVector<Fid> ProjectIndex::override_unit(Fid tu) {
    llvm::SmallVector<Fid> affected;
    if(!base_unit(tu)) {
        return affected;
    }
    if(auto persisted = persisted_manifest(tu)) {
        for(auto version: llvm::make_first_range(persisted->contributions)) {
            affected.push_back(files.version(version).fid);
        }
        for(auto& use: persisted->instantiations) {
            affected.push_back(files.version(use.file).fid);
        }
    } else {
        // What a lost manifest touched is unknown: every held view may
        // serve the unit's entries.
        for(auto& [file, shard]: shards) {
            if(shard.loaded()) {
                affected.push_back(file);
            }
        }
        for(auto& [file, held]: instantiations) {
            if(held.loaded()) {
                affected.push_back(file);
            }
        }
    }
    hide_base_unit(tu);
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
    for_each_use(path_id, [&](Fid, std::uint64_t use) { hashes.push_back(use); });
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
    for_each_contributed(file,
                         [&](Fid, const Contributed& contributed) { add(contributed.version); });
    return generations;
}

std::optional<std::uint64_t> ProjectIndex::variant_generation(Fid file,
                                                              std::uint64_t variant) const {
    std::uint64_t hash = 0;
    for_each_contributed(file, [&](Fid, const Contributed& contributed) {
        if(contributed.variant == variant) {
            hash = files.version(contributed.version).content_hash;
        }
    });
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
    return manifests.contains(tu) || base_unit(tu);
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
    if(!base) {
        return;
    }
    for(auto& unit: base->columns->units) {
        if(!hidden.contains(unit.file)) {
            visit(base->remap[unit.file]);
        }
    }
}

std::uint32_t ProjectIndex::unit_count() const {
    std::uint32_t count = 0;
    for_each_unit([&](Fid) { count += 1; });
    return count;
}

std::optional<Fid> ProjectIndex::find_unit(std::uint64_t hash) const {
    if(!base_units_hashed && base) {
        base_units_hashed = true;
        for(auto& unit: base->columns->units) {
            auto tu = base->remap[unit.file];
            units.try_emplace(unit_hash(tu), tu);
        }
    }
    auto it = units.find(hash);
    return it != units.end() && has_manifest(it->second) ? std::optional(it->second) : std::nullopt;
}

bool ProjectIndex::current_run(const Producer& producer) const {
    auto tu = find_unit(producer.unit);
    if(!tu) {
        return false;
    }
    if(auto it = manifests.find(*tu); it != manifests.end()) {
        return it->second.built_at == producer.run;
    }
    auto* unit = base_unit(*tu);
    return unit && unit->run == producer.run;
}

void ProjectIndex::for_each_contribution(
    Fid file,
    llvm::function_ref<void(Fid tu, std::uint64_t variant)> visit) const {
    for_each_contributed(file, [&](Fid tu, const Contributed& contributed) {
        visit(tu, contributed.variant);
    });
}

void ProjectIndex::for_each_contributed(
    Fid file,
    llvm::function_ref<void(Fid tu, const Contributed&)> visit) const {
    if(auto* contributed = base ? base->contributed(file) : nullptr) {
        for(auto& entry: contributed->variants) {
            if(auto version = runtime_version(entry.version)) {
                for_each_base_unit(entry.units, [&](Fid tu) {
                    visit(tu, {.variant = entry.variant, .version = *version});
                });
            }
        }
    }
    if(auto it = contributions.find(file); it != contributions.end()) {
        for(auto& [tu, contributed]: it->second) {
            visit(tu, contributed);
        }
    }
}

void ProjectIndex::for_each_use(Fid file,
                                llvm::function_ref<void(Fid tu, std::uint64_t use)> visit) const {
    if(auto* used = base ? base->used(file) : nullptr) {
        for(auto& entry: used->uses) {
            for_each_base_unit(entry.units, [&](Fid tu) { visit(tu, entry.key); });
        }
    }
    if(auto it = instantiation_uses.find(file); it != instantiation_uses.end()) {
        for(auto [tu, use]: it->second) {
            visit(tu, use);
        }
    }
}

void ProjectIndex::for_each_indexed_file(llvm::function_ref<void(Fid)> visit) const {
    llvm::DenseSet<Fid> seen;
    if(base) {
        for(auto& contributed: base->columns->contributions) {
            auto file = base->remap[contributed.file];
            bool live = false;
            for_each_contribution(file, [&](Fid, std::uint64_t) { live = true; });
            if(live && seen.insert(file).second) {
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
    if(auto* units = base ? base->absent(place) : nullptr) {
        for_each_base_unit(units->units, visit);
    }
    if(auto it = absent_lookups.find(place); it != absent_lookups.end()) {
        llvm::for_each(it->second, visit);
    }
}

void ProjectIndex::for_each_importer(Fid input, llvm::function_ref<void(Fid)> visit) const {
    if(auto* units = base ? base->imported(input) : nullptr) {
        for_each_base_unit(units->units, visit);
    }
    if(auto it = importers.find(input); it != importers.end()) {
        llvm::for_each(it->second, visit);
    }
}

void ProjectIndex::for_each_file_of_local(SymbolHash hash,
                                          Fid anchor,
                                          llvm::function_ref<void(Fid)> visit) const {
    llvm::SmallVector<llvm::SmallVector<Fid, 4>> sets;
    if(auto* entry = base ? base->shared_local(hash) : nullptr) {
        for(auto& listed: entry->sets) {
            bool named = false;
            for_each_base_unit(listed.units, [&](Fid) { named = true; });
            auto ids = read_file_set(listed.files);
            if(!named || !ids) {
                continue;
            }
            auto& set = sets.emplace_back();
            for(auto id: *ids) {
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

void ProjectIndex::lose(Fid unit, Lost what) const {
    bool first = lost_indexes.empty() && lost_uses.empty();
    (what == Lost::Index ? lost_indexes : lost_uses).insert(unit);
    if(first) {
        on_data_lost.emit();
    }
}

Shard ProjectIndex::fetch_shard(Fid file) const {
    auto live = live_variants(file);
    if(live.empty() || !db) {
        return {};
    }
    auto buffer = db->read(BlobKind::Shard, key_of(file)).buffer;
    auto shard = buffer ? Shard::from_buffer(std::move(buffer)) : Shard();
    // A blob can verify yet miss a contributed variant: a crash or a failed
    // write left a manifest newer than its shard. A variant hashes its
    // bytes, content generation included, so the blob holding it is the
    // whole check.
    bool serves = false;
    bool complete = true;
    for_each_contribution(file, [&](Fid tu, std::uint64_t variant) {
        if(shard.loaded() && shard.has_variant(variant)) {
            serves = true;
        } else {
            complete = false;
            lose(tu, Lost::Index);
        }
    });
    if(!complete) {
        LOG_INFO("The shard of {} misses variants units contributed", files.resolve(file));
    }
    if(!serves) {
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
    auto buffer = db->read(BlobKind::InstantiationShard, key_of(file)).buffer;
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
    // unit's instantiations resolved there.
    for_each_use(file, [&](Fid tu, std::uint64_t use) {
        if(!loaded.loaded() || !loaded.has_use(use)) {
            lose(tu, Lost::Use);
        }
    });
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

llvm::SmallVector<Fid> ProjectIndex::lost_units() const {
    llvm::SmallVector<Fid> lost(lost_indexes.begin(), lost_indexes.end());
    for(auto tu: lost_uses) {
        if(!lost_indexes.contains(tu)) {
            lost.push_back(tu);
        }
    }
    return lost;
}

IndexReport ProjectIndex::take_lost() {
    IndexReport report;
    for(auto tu: lost_units()) {
        report.add_reindex(tu);
    }
    // A unit whose index cannot serve loses its manifest, which retires its
    // contributions to every file it touched.
    llvm::SmallVector<Fid> unservable(lost_indexes.begin(), lost_indexes.end());
    for(auto tu: unservable) {
        drop(tu, report);
    }
    lost_indexes.clear();
    lost_uses.clear();
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
