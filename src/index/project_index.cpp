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

/// A table derived from the manifests: file -> (TU -> the hash it names there).
using UnitHashes = llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, std::uint64_t, 2>>;

/// Drop `unit`'s entry for `file`, and the file once no unit names it. One
/// file's two persisted versions in a manifest reach here twice.
void erase_unit(UnitHashes& table, Fid file, Fid unit) {
    auto it = table.find(file);
    if(it == table.end()) {
        return;
    }
    it->second.erase(unit);
    if(it->second.empty()) {
        table.erase(it);
    }
}

/// The distinct hashes the units name for `file`, ascending.
llvm::SmallVector<std::uint64_t> distinct_hashes(const UnitHashes& table, Fid file) {
    llvm::SmallVector<std::uint64_t> hashes;
    if(auto it = table.find(file); it != table.end()) {
        llvm::append_range(hashes, llvm::make_second_range(it->second));
    }
    llvm::sort(hashes);
    hashes.erase(llvm::unique(hashes), hashes.end());
    return hashes;
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
    bound->remap.reserve(bound->paths.size());
    for(std::uint32_t i = 0; i < bound->paths.size(); i += 1) {
        bound->remap.push_back(files.intern(local(bound->paths[i])));
    }
    bound->buffer = std::move(blob);
    base = std::move(bound);
    return true;
}

std::expected<void, llvm::StringRef>
    ProjectIndex::adopt_file_versions(llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins) {
    if(!base) {
        return {};
    }
    auto& blob = *base;
    auto count = blob.version_ids.size();

    // Every value check runs before the first mutation: a blob rejected
    // halfway through would otherwise leave partial state behind — file
    // versions whose corrupt stat stamps feed the freshness fast path —
    // while the caller treats the failed load as "no index on disk".
    for(std::uint32_t i = 0; i < count; i += 1) {
        if(blob.version_paths[i].empty()) {
            return std::unexpected("empty file version path");
        }
    }
    // Every id below becomes a DenseMap or DenseSet key, first in the
    // duplicate checks here and then in the id maps — see reserved_key.
    // The counter is one hand-out away from becoming a key itself, and
    // the writer hands ids out from it, so ids must sit below it — a
    // bound that (with the sentinels at the top of the id space) also
    // keeps every id non-reserved.
    if(reserved_key(blob.next_version_id) || blob.next_version_id > max_persisted_versions) {
        return std::unexpected("file version counter out of range");
    }
    for(auto id: blob.version_ids) {
        if(id >= blob.next_version_id) {
            return std::unexpected("file version id past the counter");
        }
    }
    for(auto version: blob.manifest_versions) {
        if(reserved_key(version)) {
            return std::unexpected("reserved manifest pin");
        }
    }
    // Ids and (path, hash) pairs are both map keys in the writer, so a
    // repeat of either marks a corrupt blob: a repeated id in particular
    // would map to the earlier pair while its record names the later
    // path, attributing contributions to the wrong file.
    llvm::DenseSet<std::uint32_t> blob_version_ids(blob.version_ids.begin(),
                                                   blob.version_ids.end());
    if(blob_version_ids.size() != count) {
        return std::unexpected("duplicate file version id");
    }
    llvm::DenseSet<std::pair<llvm::StringRef, std::uint64_t>> blob_versions;
    for(std::uint32_t i = 0; i < count; i += 1) {
        if(!blob_versions.insert({blob.version_paths[i], blob.version_hashes[i]}).second) {
            return std::unexpected("duplicate file version");
        }
    }
    // The writer only pins manifests whose tu_version survived the same save's
    // garbage collection, so an unresolvable pin marks a corrupt blob.
    for(auto version: blob.manifest_versions) {
        if(!blob_version_ids.contains(version)) {
            return std::unexpected("manifest pin names an unknown file version");
        }
    }

    // Two spellings of one file intern to the same version; the ids both
    // stay mapped to it.
    next_persisted_id = blob.next_version_id;
    for(std::uint32_t i = 0; i < count; i += 1) {
        auto path_id = files.intern(local(blob.version_paths[i]));
        auto id = files.intern_version(path_id, blob.version_hashes[i]);
        runtime_ids.try_emplace(blob.version_ids[i], id);
        persisted_ids.try_emplace(id, blob.version_ids[i]);
    }
    for(std::uint32_t k = 0; k < blob.manifest_versions.size(); k += 1) {
        manifest_pins[runtime_ids.find(blob.manifest_versions[k])->second] =
            blob.manifest_generations[k];
    }
    return {};
}

bool ProjectIndex::verify_bitmaps() const {
    if(!base) {
        return true;
    }
    auto in_paths = [&](const std::optional<Bitmap>& decoded) {
        return decoded && (decoded->isEmpty() || decoded->maximum() < base->paths.size());
    };
    for(std::uint32_t entry = 0; entry < base->count(); entry += 1) {
        if(!in_paths(base->bitmap(entry)) || !in_paths(base->instantiation_bitmap(entry))) {
            return false;
        }
    }
    return true;
}

std::expected<void, llvm::StringRef>
    ProjectIndex::load_table(llvm::StringRef data,
                             llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins) {
    if(!bind_table(llvm::MemoryBuffer::getMemBufferCopy(data))) {
        return std::unexpected("not a readable project table");
    }
    if(!verify_bitmaps()) {
        base.reset();
        return std::unexpected("reference bitmap does not decode");
    }
    if(auto adopted = adopt_file_versions(manifest_pins); !adopted) {
        base.reset();
        return adopted;
    }
    adopt_base();
    return {};
}

void ProjectIndex::adopt_base() {
    generation = base->generation;
    search_generation = base->search_generation;
    pending_symbols.clear();
    pending_symbols.insert(base->search_pending.begin(), base->search_pending.end());
    pending_files.clear();
    for(auto file: base->search_pending_files) {
        pending_files.insert(base->remap[file]);
    }
}

const Symbol* ProjectIndex::held(SymbolHash hash) const {
    if(auto it = changed.find(hash); it != changed.end()) {
        return &it->second;
    }
    if(auto it = written.find(hash); it != written.end()) {
        return &it->second;
    }
    return nullptr;
}

std::optional<SymbolView> ProjectIndex::find_symbol(SymbolHash hash) const {
    if(auto* symbol = held(hash)) {
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
    if(auto* symbol = held(hash)) {
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
    if(auto* symbol = held(hash)) {
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
    if(auto* symbol = held(hash)) {
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
        auto hash = base->hashes[entry];
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

void ProjectIndex::serialize_table(llvm::raw_ostream& os) {
    // The entries changed since the last write join the ones still held
    // aside from a write that was never persisted; the newer entry wins.
    for(auto& [hash, symbol]: changed) {
        written[hash] = std::move(symbol);
    }
    changed.clear();

    // The blob carries only the versions some manifest still references —
    // the persisted form's garbage collection. The shared table itself is
    // left alone: other consumers may still anchor checks on a version the
    // index dropped, and ids are never reused either way.
    llvm::DenseSet<VersionID> referenced;
    for(auto& manifest: llvm::make_second_range(manifests)) {
        for_each_version(manifest,
                         [&](std::uint32_t version) { referenced.insert(VersionID{version}); });
    }

    ProjectTableBlob blob;
    blob.format_version = index_format_version;
    blob.generation = generation;

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
    }
    blob.next_version_id = next_persisted_id;

    blob.manifest_versions.reserve(manifests.size());
    blob.manifest_generations.reserve(manifests.size());
    for(auto& manifest: llvm::make_second_range(manifests)) {
        blob.manifest_versions.push_back(persisted_ids.find(manifest.tu_version)->second);
        blob.manifest_generations.push_back(manifest.table_generation);
    }

    // The base's path table stays as it is, so its entries' bitmap images
    // copy verbatim; files the written entries reference beyond it are
    // appended.
    llvm::DenseMap<Fid, std::uint32_t> id_of;
    if(base) {
        blob.paths.reserve(base->paths.size());
        for(std::uint32_t i = 0; i < base->paths.size(); i += 1) {
            blob.paths.emplace_back(base->paths[i]);
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
        if(k == keys.size() || (base_entry < base_count && base->hashes[base_entry] < keys[k])) {
            entries.push_back({base->hashes[base_entry], base_entry, nullptr});
            base_entry += 1;
            continue;
        }
        if(base_entry < base_count && base->hashes[base_entry] == keys[k]) {
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

    std::vector<std::pair<std::uint32_t, Bitmap>> reverse;
    reverse.reserve(contributions.size());
    for(auto& [file, units]: contributions) {
        Bitmap tus;
        for(auto tu: llvm::make_first_range(units)) {
            tus.add(path_id(tu));
        }
        reverse.emplace_back(path_id(file), std::move(tus));
    }
    std::ranges::sort(reverse, {}, [](const auto& entry) { return entry.first; });
    for(auto& [file, tus]: reverse) {
        blob.contributed_files.push_back(file);
        append_file_set(blob.contributors, tus);
        blob.contributor_ends.push_back(static_cast<std::uint32_t>(blob.contributors.size()));
    }

    llvm::SmallVector<SymbolHash> symbols(llvm::make_first_range(shared_local_sets));
    llvm::sort(symbols);
    for(auto symbol: symbols) {
        for(auto& set: llvm::make_first_range(shared_local_sets.find(symbol)->second)) {
            Bitmap listed;
            for(auto file: set) {
                listed.add(path_id(file));
            }
            append_file_set(blob.shared_local_files, listed);
            blob.shared_local_file_ends.push_back(
                static_cast<std::uint32_t>(blob.shared_local_files.size()));
        }
        blob.shared_local_symbols.push_back(symbol);
        blob.shared_local_set_ends.push_back(
            static_cast<std::uint32_t>(blob.shared_local_file_ends.size()));
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

    serialize_blob(blob, os);
}

void ProjectIndex::rebase(std::unique_ptr<llvm::MemoryBuffer> blob) {
    [[maybe_unused]] bool bound = bind_table(std::move(blob));
    assert(bound && "a freshly serialized project table must bind");
    written.clear();
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
        next_persisted_id += 1;
    }
    return it->second;
}

llvm::SmallVector<Fid> ProjectIndex::apply_manifest(Fid tu_path_id, TUManifest manifest) {
    llvm::SmallVector<Fid> affected = remove_manifest(tu_path_id);

    for(auto& [version, hash]: manifest.contributions) {
        auto path_id = files.version(version).fid;
        contributions[path_id][tu_path_id] = hash;
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
        shared_local_sets[local.symbol][listed_files(files, local)] += 1;
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

llvm::SmallVector<Fid> ProjectIndex::remove_manifest(Fid tu_path_id) {
    llvm::SmallVector<Fid> affected;
    auto it = manifests.find(tu_path_id);
    if(it == manifests.end()) {
        return affected;
    }

    for(auto& [version, hash]: it->second.contributions) {
        auto path_id = files.version(version).fid;
        erase_unit(contributions, path_id, tu_path_id);
        affected.push_back(path_id);
    }
    for(auto version: it->second.absent) {
        auto probe_it = absent_lookups.find(files.version(version).fid);
        probe_it->second.erase(tu_path_id);
        if(probe_it->second.empty()) {
            absent_lookups.erase(probe_it);
        }
    }
    // Modules built at different times may have read different versions
    // of one file: the first of them already erased it.
    for(auto version: it->second.imports) {
        auto importer_it = importers.find(files.version(version).fid);
        if(importer_it == importers.end()) {
            continue;
        }
        importer_it->second.erase(tu_path_id);
        if(importer_it->second.empty()) {
            importers.erase(importer_it);
        }
    }
    for(auto& local: it->second.shared_locals) {
        auto sets = shared_local_sets.find(local.symbol);
        auto set = sets->second.find(listed_files(files, local));
        set->second -= 1;
        if(set->second == 0) {
            sets->second.erase(set);
        }
        if(sets->second.empty()) {
            shared_local_sets.erase(sets);
        }
    }
    for(auto& use: it->second.instantiations) {
        auto path_id = files.version(use.file).fid;
        erase_unit(instantiation_uses, path_id, tu_path_id);
        affected.push_back(path_id);
    }
    units.erase(unit_hash(tu_path_id));
    manifests.erase(it);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
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
        search_file_changed(file);
    }
}

void ProjectIndex::search_file_changed(Fid file) {
    pending_files.insert(file);
    if(changed_during_search_build) {
        changed_during_search_build->files.insert(file);
    }
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(Fid path_id) const {
    return distinct_hashes(contributions, path_id);
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_uses(Fid path_id) const {
    return distinct_hashes(instantiation_uses, path_id);
}

const TUManifest* ProjectIndex::held_manifest(Fid tu) const {
    auto it = manifests.find(tu);
    return it != manifests.end() ? &it->second : nullptr;
}

void ProjectIndex::for_each_manifest(
    llvm::function_ref<void(Fid tu, const TUManifest&)> visit) const {
    for(auto& [tu, manifest]: manifests) {
        visit(tu, manifest);
    }
}

std::uint32_t ProjectIndex::manifest_count() const {
    return manifests.size();
}

void ProjectIndex::for_each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const {
    if(!db) {
        for_each_contribution(file, [&](Fid tu, std::uint64_t) { visit(tu); });
        return;
    }
    if(!base) {
        return;
    }
    if(auto tus = base->contributors_of(file)) {
        for_each_remapped(*base, *tus, visit);
    }
}

void ProjectIndex::for_each_contribution(
    Fid file,
    llvm::function_ref<void(Fid tu, std::uint64_t variant)> visit) const {
    if(auto it = contributions.find(file); it != contributions.end()) {
        for(auto [tu, variant]: it->second) {
            visit(tu, variant);
        }
    }
}

void ProjectIndex::for_each_absent_lookup(Fid place, llvm::function_ref<void(Fid)> visit) const {
    if(auto it = absent_lookups.find(place); it != absent_lookups.end()) {
        llvm::for_each(it->second, visit);
    }
}

void ProjectIndex::for_each_importer(Fid input, llvm::function_ref<void(Fid)> visit) const {
    if(auto it = importers.find(input); it != importers.end()) {
        llvm::for_each(it->second, visit);
    }
}

void ProjectIndex::for_each_file_of_local(SymbolHash hash,
                                          Fid anchor,
                                          llvm::function_ref<void(Fid)> visit) const {
    llvm::SmallVector<llvm::SmallVector<Fid, 4>> sets;
    if(!db) {
        if(auto it = shared_local_sets.find(hash); it != shared_local_sets.end()) {
            for(auto& set: llvm::make_first_range(it->second)) {
                sets.emplace_back(set.begin(), set.end());
            }
        }
    } else if(base) {
        for(auto& listed: base->shared_local_sets_of(hash)) {
            auto& set = sets.emplace_back();
            for_each_remapped(*base, listed, [&](Fid file) { set.push_back(file); });
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

const TUManifest* ProjectIndex::manifest(Fid tu) const {
    if(!db) {
        auto it = manifests.find(tu);
        return it != manifests.end() ? &it->second : nullptr;
    }
    auto [it, inserted] = fetched_manifests.try_emplace(tu);
    if(inserted) {
        // A reader adopts a manifest only under the generation the bound project table
        // pins for it, as the writer's load does; the versions it names
        // stay persisted ids.
        auto pinned = [&](const TUManifest& manifest) {
            auto pin = std::ranges::find(base->manifest_versions, manifest.tu_version.raw);
            return pin != base->manifest_versions.end() &&
                   base->manifest_generations[pin - base->manifest_versions.begin()] ==
                       manifest.table_generation;
        };
        if(auto blob = db->read(BlobKind::TUManifest, key_of(tu))) {
            if(auto manifest = deserialize_manifest(blob.buffer->getBuffer());
               manifest && pinned(*manifest)) {
                it->second = std::move(*manifest);
            }
        }
    }
    return it->second ? &*it->second : nullptr;
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
    // live in the contributing TUs' manifests instead. The generation gate
    // above dedups divergent revisions; agreeing TUs collapse in the
    // projection's dedup.
    if(auto manifest_it = manifests.find(file); manifest_it != manifests.end()) {
        append(manifest_it->second);
        return edges;
    }
    if(auto contribution_it = contributions.find(file); contribution_it != contributions.end()) {
        for(auto tu: llvm::make_first_range(contribution_it->second)) {
            if(auto manifest_it = manifests.find(tu); manifest_it != manifests.end()) {
                append(manifest_it->second);
            }
        }
    }
    return edges;
}

const Shard* ProjectIndex::shard(Fid file) const {
    auto it = shards.find(file);
    if(it != shards.end()) {
        return it->second.loaded() ? &it->second : nullptr;
    }
    if(!db) {
        return nullptr;
    }
    // A miss is remembered as an empty entry so the database is asked
    // once per file.
    auto& slot = shards[file];
    if(auto blob = db->read(BlobKind::Shard, key_of(file))) {
        slot = Shard::from_buffer(std::move(blob.buffer));
    }
    return slot.loaded() ? &slot : nullptr;
}

void ProjectIndex::for_each_shard(llvm::function_ref<void(Fid, const Shard&)> visit) const {
    for(auto& [file, shard]: shards) {
        if(shard.loaded()) {
            visit(file, shard);
        }
    }
}

std::uint32_t ProjectIndex::shard_count() const {
    return shards.size();
}

const InstantiationShard* ProjectIndex::instantiation_shard(Fid file) const {
    auto it = instantiations.find(file);
    if(it != instantiations.end()) {
        return it->second.loaded() ? &it->second : nullptr;
    }
    if(!db) {
        return nullptr;
    }
    auto& slot = instantiations[file];
    if(auto blob = db->read(BlobKind::InstantiationShard, key_of(file))) {
        slot = InstantiationShard::from_buffer(std::move(blob.buffer));
    }
    return slot.loaded() ? &slot : nullptr;
}

void ProjectIndex::for_each_instantiation_shard(
    llvm::function_ref<void(Fid, const InstantiationShard&)> visit) const {
    for(auto& [file, instantiation_shard]: instantiations) {
        if(instantiation_shard.loaded()) {
            visit(file, instantiation_shard);
        }
    }
}

bool ProjectIndex::bind_search(std::unique_ptr<llvm::MemoryBuffer> blob) {
    SearchIndex built;
    if(!built.load(std::move(blob)) || built.generation() != search_generation) {
        return false;
    }
    search = std::move(built);
    return true;
}

bool ProjectIndex::open(BlobDatabase& db) {
    auto table = db.read(BlobKind::ProjectTable, "global");
    if(!table || !bind_table(std::move(table.buffer))) {
        return false;
    }
    adopt_base();
    if(auto blob = db.read(BlobKind::SearchIndex, "search")) {
        bind_search(std::move(blob.buffer));
    }
    this->db = &db;
    return true;
}

}  // namespace clice::index
