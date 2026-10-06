module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.project_index;
import :index.serialization;
import :support.logging;

namespace clice::index {

namespace {

/// Upper bound on the persisted FileVersion id space a blob may claim:
/// the writer hands ids out from the blob's counter, so a garbage one just
/// below the reserved keys would soon hand one out. The cap is far beyond
/// any lineage a writer accumulates (ids grow only with newly persisted
/// (file, content-hash) pairs), and a lineage that ever drifts there is
/// better compacted by the rebuild the rejection triggers.
constexpr inline std::uint32_t max_persisted_versions = 1u << 24;

/// The global layer's persisted form, read in place: the FileVersion table
/// as parallel columns, the symbol table as parallel columns in hash order
/// with its names, arguments and reference bitmaps in arenas, and the path
/// table those columns index.
struct GlobalBlob {
    std::uint32_t format_version = 0;

    /// See ProjectIndex::global_generation.
    std::uint64_t generation = 0;

    std::uint32_t next_fv_id = 0;

    std::vector<std::uint32_t> fv_ids;
    std::vector<std::string> fv_paths;
    std::vector<std::uint64_t> fv_hashes;

    /// Path id -> path for every id the symbol columns reference. Ids
    /// are dense and stable across writes: a path keeps its id for as
    /// long as the blob lineage lives, so a base row's bitmap image is
    /// copied into the next blob verbatim.
    std::vector<std::string> paths;

    /// The external symbol table, sorted by hash. Reference bitmaps are
    /// portable roaring images back to back, one end offset per row.
    std::vector<std::uint64_t> sym_hashes;
    std::string sym_names;
    std::vector<std::uint32_t> sym_name_ends;
    std::string sym_args;
    std::vector<std::uint32_t> sym_args_ends;
    std::vector<std::uint64_t> sym_parents;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::uint16_t> sym_flags;
    /// Path ids, `no_file` for a symbol without a declaring row.
    std::vector<std::uint32_t> sym_files;
    std::vector<std::uint32_t> sym_reference_counts;
    std::vector<std::uint32_t> sym_bitmap_ends;
    std::vector<std::uint8_t> sym_bitmaps;

    /// tu_fv -> generation stamp of every manifest current at this save.
    /// The loader adopts a manifest blob only on an exact stamp match, so
    /// a manifest whose write failed while the global landed (or one that
    /// outran a lost global write) reads as lost and its TU reindexes,
    /// instead of an older on-disk manifest serving as current.
    std::vector<std::uint32_t> manifest_fvs;
    std::vector<std::uint64_t> manifest_gens;

    /// The generation of the search blob this table was saved with, and
    /// the rows changed since that blob was built.
    std::uint64_t search_generation = 0;
    std::vector<std::uint64_t> search_pending;

    /// The reverse include graph, for readers holding no manifests: every
    /// file some TU contributed rows to (path ids, ascending), and those
    /// TUs' path ids as portable roaring images back to back.
    std::vector<std::uint32_t> contributed_files;
    std::vector<std::uint32_t> contributor_ends;
    std::vector<std::uint8_t> contributors;
};

using BlobView = kota::codec::fbs::table_view<GlobalBlob>;

llvm::StringRef slice(llvm::StringRef arena, llvm::ArrayRef<std::uint32_t> ends, std::uint32_t i) {
    auto begin = i == 0 ? 0 : ends[i - 1];
    return arena.slice(begin, ends[i]);
}

}  // namespace

/// The bound global blob: its columns, and its path table mapped to the
/// file table's ids.
struct ProjectIndex::Base {
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    std::uint64_t generation = 0;
    std::uint32_t next_fv_id = 0;
    llvm::ArrayRef<std::uint32_t> fv_ids;
    kota::codec::fbs::array_view<std::string> fv_paths;
    llvm::ArrayRef<std::uint64_t> fv_hashes;

    kota::codec::fbs::array_view<std::string> paths;
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::StringRef names;
    llvm::ArrayRef<std::uint32_t> name_ends;
    llvm::StringRef args;
    llvm::ArrayRef<std::uint32_t> args_ends;
    llvm::ArrayRef<std::uint64_t> parents;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint32_t> files;
    llvm::ArrayRef<std::uint32_t> reference_counts;
    llvm::ArrayRef<std::uint32_t> bitmap_ends;
    llvm::ArrayRef<std::uint8_t> bitmaps;

    llvm::ArrayRef<std::uint32_t> manifest_fvs;
    llvm::ArrayRef<std::uint64_t> manifest_gens;

    std::uint64_t search_generation = 0;
    llvm::ArrayRef<std::uint64_t> search_pending;

    llvm::ArrayRef<std::uint32_t> contributed_files;
    llvm::ArrayRef<std::uint32_t> contributor_ends;
    llvm::ArrayRef<std::uint8_t> contributors;

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

    /// File table id -> index into contributed_files, built on first use.
    mutable llvm::DenseMap<Fid, std::uint32_t> contributed_index;

    std::uint32_t count() const {
        return static_cast<std::uint32_t>(hashes.size());
    }

    std::optional<std::uint32_t> find(SymbolHash hash) const {
        auto it = std::ranges::lower_bound(hashes, hash);
        if(it == hashes.end() || *it != hash) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - hashes.begin());
    }

    llvm::StringRef name(std::uint32_t doc) const {
        return slice(names, name_ends, doc);
    }

    llvm::StringRef arguments(std::uint32_t doc) const {
        return slice(args, args_ends, doc);
    }

    /// The `i`th of the images stored back to back in `arena`.
    static llvm::ArrayRef<std::uint8_t> image(llvm::ArrayRef<std::uint8_t> arena,
                                              llvm::ArrayRef<std::uint32_t> ends,
                                              std::uint32_t i) {
        auto begin = i == 0 ? 0 : ends[i - 1];
        return arena.slice(begin, ends[i] - begin);
    }

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t doc) const {
        return image(bitmaps, bitmap_ends, doc);
    }

    std::optional<Bitmap> bitmap(std::uint32_t doc) const {
        auto bytes = image(doc);
        return view_bitmap(bytes.data(), bytes.size());
    }

    /// The TUs that contributed rows to `file`, as path ids; nullopt when
    /// none did or the image fails to decode.
    std::optional<Bitmap> contributors_of(Fid file) const {
        if(contributed_index.empty()) {
            for(std::uint32_t i = 0; i < contributed_files.size(); i += 1) {
                contributed_index.try_emplace(remap[contributed_files[i]], i);
            }
        }
        auto it = contributed_index.find(file);
        if(it == contributed_index.end()) {
            return std::nullopt;
        }
        auto bytes = image(contributors, contributor_ends, it->second);
        return view_bitmap(bytes.data(), bytes.size());
    }

    /// The portable name of the file a persisted version id names.
    std::optional<llvm::StringRef> version_path(std::uint32_t persisted) const {
        auto it = std::ranges::lower_bound(fv_ids, persisted);
        if(it == fv_ids.end() || *it != persisted) {
            return std::nullopt;
        }
        return to_ref(fv_paths[it - fv_ids.begin()]);
    }

    SymbolIdentity identity(std::uint32_t doc) const {
        auto file = files[doc];
        return {
            .name = name(doc),
            .args = arguments(doc),
            .parent = parents[doc],
            .kind = SymbolKind(kinds[doc]),
            .scope = SymbolScope::External,
            .flags = static_cast<SymbolFlags>(flags[doc]),
            .file = file == no_file ? no_file : remap[file].raw,
        };
    }

    /// Bind the columns of a verified blob, checking they line up.
    std::expected<void, llvm::StringRef> bind(BlobView root);
};

std::expected<void, llvm::StringRef> ProjectIndex::Base::bind(BlobView root) {
    if(root[&GlobalBlob::format_version] != index_format_version) {
        return std::unexpected("written by another index format version");
    }
    generation = root[&GlobalBlob::generation];
    next_fv_id = root[&GlobalBlob::next_fv_id];
    fv_ids = to_array_ref(root[&GlobalBlob::fv_ids]);
    fv_paths = root[&GlobalBlob::fv_paths];
    fv_hashes = to_array_ref(root[&GlobalBlob::fv_hashes]);
    paths = root[&GlobalBlob::paths];
    hashes = to_array_ref(root[&GlobalBlob::sym_hashes]);
    names = to_ref(root[&GlobalBlob::sym_names]);
    name_ends = to_array_ref(root[&GlobalBlob::sym_name_ends]);
    args = to_ref(root[&GlobalBlob::sym_args]);
    args_ends = to_array_ref(root[&GlobalBlob::sym_args_ends]);
    parents = to_array_ref(root[&GlobalBlob::sym_parents]);
    kinds = to_array_ref(root[&GlobalBlob::sym_kinds]);
    flags = to_array_ref(root[&GlobalBlob::sym_flags]);
    files = to_array_ref(root[&GlobalBlob::sym_files]);
    reference_counts = to_array_ref(root[&GlobalBlob::sym_reference_counts]);
    bitmap_ends = to_array_ref(root[&GlobalBlob::sym_bitmap_ends]);
    bitmaps = to_array_ref(root[&GlobalBlob::sym_bitmaps]);
    manifest_fvs = to_array_ref(root[&GlobalBlob::manifest_fvs]);
    manifest_gens = to_array_ref(root[&GlobalBlob::manifest_gens]);
    search_generation = root[&GlobalBlob::search_generation];
    search_pending = to_array_ref(root[&GlobalBlob::search_pending]);
    contributed_files = to_array_ref(root[&GlobalBlob::contributed_files]);
    contributor_ends = to_array_ref(root[&GlobalBlob::contributor_ends]);
    contributors = to_array_ref(root[&GlobalBlob::contributors]);

    auto version_count = fv_ids.size();
    if(fv_paths.size() != version_count || fv_hashes.size() != version_count) {
        return std::unexpected("file version columns do not line up");
    }
    if(!std::ranges::is_sorted(fv_ids)) {
        return std::unexpected("file version ids are not ascending");
    }
    if(contributor_ends.size() != contributed_files.size() ||
       !monotone_ends(contributor_ends, contributors.size())) {
        return std::unexpected("contributor columns do not line up");
    }
    for(std::size_t i = 0; i < contributed_files.size(); i += 1) {
        if(contributed_files[i] >= paths.size() ||
           (i > 0 && contributed_files[i] <= contributed_files[i - 1])) {
            return std::unexpected("contributed files are not ascending path ids");
        }
    }
    if(manifest_fvs.size() != manifest_gens.size()) {
        return std::unexpected("manifest pin columns do not line up");
    }
    auto count = hashes.size();
    if(name_ends.size() != count || args_ends.size() != count || parents.size() != count ||
       kinds.size() != count || flags.size() != count || files.size() != count ||
       reference_counts.size() != count || bitmap_ends.size() != count) {
        return std::unexpected("symbol columns do not line up");
    }
    if(!monotone_ends(name_ends, names.size()) || !monotone_ends(args_ends, args.size()) ||
       !monotone_ends(bitmap_ends, bitmaps.size())) {
        return std::unexpected("symbol arenas do not line up");
    }
    // Hashes, parents and pending rows become table keys downstream, which
    // reserve two sentinel values; a repeated hash would let one row shadow
    // another.
    for(std::size_t i = 0; i < count; i += 1) {
        if(reserved_key(hashes[i]) || (i > 0 && hashes[i] <= hashes[i - 1])) {
            return std::unexpected("symbol hashes are not ascending");
        }
        if(reserved_key(parents[i])) {
            return std::unexpected("reserved symbol parent");
        }
        if(files[i] != no_file && files[i] >= paths.size()) {
            return std::unexpected("symbol file outside the path table");
        }
    }
    for(auto hash: search_pending) {
        if(reserved_key(hash)) {
            return std::unexpected("reserved pending search row");
        }
    }
    // The writer only emits interned paths, which are never empty; an
    // empty entry marks a corrupt blob and must not become a real pool
    // entry.
    for(std::uint32_t i = 0; i < paths.size(); i += 1) {
        if(paths[i].empty()) {
            return std::unexpected("empty symbol path");
        }
    }
    return {};
}

ProjectIndex::ProjectIndex() = default;
ProjectIndex::~ProjectIndex() = default;
ProjectIndex::ProjectIndex(ProjectIndex&&) noexcept = default;
ProjectIndex& ProjectIndex::operator=(ProjectIndex&&) noexcept = default;

std::string ProjectIndex::portable(llvm::StringRef path) const {
    llvm::SmallString<256> storage;
    return path::portable(path, workspace, storage).str();
}

Spelling ProjectIndex::local(llvm::StringRef name) const {
    llvm::SmallString<256> storage;
    return Spelling::absolute(path::local(name, workspace, storage));
}

std::string ProjectIndex::key_of(const FileTable& files, Fid file) const {
    return blob_key(portable(files.resolve(file)));
}

bool ProjectIndex::bind_global(std::unique_ptr<llvm::MemoryBuffer> blob, FileTable& files) {
    if(!blob) {
        return false;
    }
    auto root = BlobView::from_bytes(blob_bytes(blob->getBuffer()));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting global blob: structural verification failed");
        return false;
    }
    auto bound = std::make_unique<Base>();
    if(auto ok = bound->bind(root); !ok) {
        LOG_DEBUG("Rejecting global blob: {}", std::string_view(ok.error()));
        return false;
    }
    // Interning only names the paths: ids left behind by a later reject
    // are inert.
    bound->remap.reserve(bound->paths.size());
    for(std::uint32_t i = 0; i < bound->paths.size(); i += 1) {
        bound->remap.push_back(files.intern(local(to_ref(bound->paths[i]))));
    }
    bound->buffer = std::move(blob);
    base = std::move(bound);
    return true;
}

std::expected<void, llvm::StringRef>
    ProjectIndex::adopt_file_versions(FileTable& files,
                                      llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins) {
    if(!base) {
        return {};
    }
    auto& blob = *base;
    auto count = blob.fv_ids.size();

    // Every value check runs before the first mutation: a blob rejected
    // halfway through would otherwise leave partial state behind — file
    // versions whose corrupt stat stamps feed the freshness fast path —
    // while the caller treats the failed load as "no index on disk".
    for(std::uint32_t i = 0; i < count; i += 1) {
        if(blob.fv_paths[i].empty()) {
            return std::unexpected("empty file version path");
        }
    }
    // Every id below becomes a DenseMap or DenseSet key, first in the
    // duplicate checks here and then in the id maps — see reserved_key.
    // The counter is one hand-out away from becoming a key itself, and
    // the writer hands ids out from it, so ids must sit below it — a
    // bound that (with the sentinels at the top of the id space) also
    // keeps every id non-reserved.
    if(reserved_key(blob.next_fv_id) || blob.next_fv_id > max_persisted_versions) {
        return std::unexpected("file version counter out of range");
    }
    for(auto id: blob.fv_ids) {
        if(id >= blob.next_fv_id) {
            return std::unexpected("file version id past the counter");
        }
    }
    for(auto fv: blob.manifest_fvs) {
        if(reserved_key(fv)) {
            return std::unexpected("reserved manifest pin");
        }
    }
    // Ids and (path, hash) pairs are both map keys in the writer, so a
    // repeat of either marks a corrupt blob: a repeated id in particular
    // would map to the earlier pair while its record names the later
    // path, attributing contributions to the wrong file.
    llvm::DenseSet<std::uint32_t> blob_fvs(blob.fv_ids.begin(), blob.fv_ids.end());
    if(blob_fvs.size() != count) {
        return std::unexpected("duplicate file version id");
    }
    llvm::DenseSet<std::pair<llvm::StringRef, std::uint64_t>> blob_versions;
    for(std::size_t i = 0; i < count; i += 1) {
        if(!blob_versions.insert({to_ref(blob.fv_paths[i]), blob.fv_hashes[i]}).second) {
            return std::unexpected("duplicate file version");
        }
    }
    // The writer only pins manifests whose tu_fv survived the same save's
    // garbage collection, so an unresolvable pin marks a corrupt blob.
    for(auto fv: blob.manifest_fvs) {
        if(!blob_fvs.contains(fv)) {
            return std::unexpected("manifest pin names an unknown file version");
        }
    }

    // Two spellings of one file intern to the same version; the ids both
    // stay mapped to it.
    next_persisted_id = blob.next_fv_id;
    for(std::size_t i = 0; i < count; i += 1) {
        auto path_id = files.intern(local(to_ref(blob.fv_paths[i])));
        auto id = files.intern_version(path_id, blob.fv_hashes[i]);
        runtime_ids.try_emplace(blob.fv_ids[i], id);
        persisted_ids.try_emplace(id, blob.fv_ids[i]);
    }
    for(std::size_t k = 0; k < blob.manifest_fvs.size(); k += 1) {
        manifest_pins[runtime_ids.find(blob.manifest_fvs[k])->second] = blob.manifest_gens[k];
    }
    return {};
}

bool ProjectIndex::verify_bitmaps() const {
    if(!base) {
        return true;
    }
    for(std::uint32_t doc = 0; doc < base->count(); doc += 1) {
        auto decoded = base->bitmap(doc);
        if(!decoded || (!decoded->isEmpty() && decoded->maximum() >= base->paths.size())) {
            return false;
        }
    }
    return true;
}

std::expected<void, llvm::StringRef>
    ProjectIndex::load_global(llvm::StringRef data,
                              FileTable& files,
                              llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins) {
    if(!bind_global(llvm::MemoryBuffer::getMemBufferCopy(data), files)) {
        return std::unexpected("not a readable global blob");
    }
    if(!verify_bitmaps()) {
        base.reset();
        return std::unexpected("reference bitmap does not decode");
    }
    if(auto adopted = adopt_file_versions(files, manifest_pins); !adopted) {
        base.reset();
        return adopted;
    }
    global_generation = base->generation;
    search_generation = base->search_generation;
    search_pending.clear();
    search_pending.insert(base->search_pending.begin(), base->search_pending.end());
    return {};
}

std::optional<SymbolIdentity> ProjectIndex::identity_of(SymbolHash hash) const {
    if(auto it = changed.find(hash); it != changed.end()) {
        return it->second.identity();
    }
    if(auto it = written.find(hash); it != written.end()) {
        return it->second.identity();
    }
    if(base) {
        if(auto doc = base->find(hash)) {
            return base->identity(*doc);
        }
    }
    return std::nullopt;
}

std::uint32_t ProjectIndex::reference_count(SymbolHash hash) const {
    if(auto it = changed.find(hash); it != changed.end()) {
        return static_cast<std::uint32_t>(it->second.reference_files.cardinality());
    }
    if(auto it = written.find(hash); it != written.end()) {
        return static_cast<std::uint32_t>(it->second.reference_files.cardinality());
    }
    if(base) {
        if(auto doc = base->find(hash)) {
            return base->reference_counts[*doc];
        }
    }
    return 0;
}

void ProjectIndex::each_reference_file(SymbolHash hash, llvm::function_ref<void(Fid)> visit) const {
    auto own = [&](const Symbol& row) {
        for(auto id: row.reference_files) {
            visit(Fid{id});
        }
    };
    if(auto it = changed.find(hash); it != changed.end()) {
        own(it->second);
        return;
    }
    if(auto it = written.find(hash); it != written.end()) {
        own(it->second);
        return;
    }
    if(!base) {
        return;
    }
    auto doc = base->find(hash);
    if(!doc) {
        return;
    }
    // A reader never proved the image; one that fails reads as no
    // references rather than crashing the query.
    auto decoded = base->bitmap(*doc);
    if(!decoded) {
        return;
    }
    for(auto id: *decoded) {
        if(id < base->remap.size()) {
            visit(base->remap[id]);
        }
    }
}

void ProjectIndex::for_each_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolIdentity&, std::uint32_t)> visit) const {
    for(auto& [hash, row]: changed) {
        if(!visit(hash,
                  row.identity(),
                  static_cast<std::uint32_t>(row.reference_files.cardinality()))) {
            return;
        }
    }
    for(auto& [hash, row]: written) {
        if(changed.contains(hash)) {
            continue;
        }
        if(!visit(hash,
                  row.identity(),
                  static_cast<std::uint32_t>(row.reference_files.cardinality()))) {
            return;
        }
    }
    if(!base) {
        return;
    }
    for(std::uint32_t doc = 0; doc < base->count(); doc += 1) {
        auto hash = base->hashes[doc];
        if(changed.contains(hash) || written.contains(hash)) {
            continue;
        }
        if(!visit(hash, base->identity(doc), base->reference_counts[doc])) {
            return;
        }
    }
}

std::size_t ProjectIndex::symbol_count() const {
    std::size_t count = base ? base->count() : 0;
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

Symbol& ProjectIndex::touch(SymbolHash hash) {
    auto [it, inserted] = changed.try_emplace(hash);
    if(!inserted) {
        return it->second;
    }
    auto& row = it->second;
    if(auto held = written.find(hash); held != written.end()) {
        row = held->second;
        return row;
    }
    if(!base) {
        return row;
    }
    auto doc = base->find(hash);
    if(!doc) {
        return row;
    }
    auto identity = base->identity(*doc);
    row.name = identity.name.str();
    row.args = identity.args.str();
    row.parent = identity.parent;
    row.kind = identity.kind;
    row.flags = identity.flags;
    row.file = identity.file;
    if(auto decoded = base->bitmap(*doc)) {
        for(auto id: *decoded) {
            if(id < base->remap.size()) {
                row.reference_files.add(base->remap[id].raw);
            }
        }
    }
    return row;
}

bool ProjectIndex::merge(const TUIndex& index,
                         llvm::ArrayRef<Fid> file_ids_map,
                         llvm::SmallVectorImpl<SymbolHash>* added) {
    // Decode and bound every reference bitmap before touching the table:
    // merged bits persist in the global blob while the result's recorded
    // versions all match the disk, so a malformed image normalized to
    // empty — or a silently dropped out-of-range id, whose relations would
    // sit in a shard the symbol's fan-out never visits — would lose
    // reference files with nothing ever rebuilding them. Either rejects
    // the whole result instead — and the reject must leave no partial
    // names or bits behind, hence the staging.
    struct StagedSymbol {
        SymbolHash hash;
        SymbolIdentity identity;
        Bitmap references;
    };

    std::vector<StagedSymbol> staged;
    bool valid = true;
    index.iterate_symbols(
        [&](SymbolHash hash, const SymbolIdentity& identity, llvm::StringRef bitmap) {
            if(identity.scope != SymbolScope::External) {
                return true;
            }
            if(reserved_key(hash) || reserved_key(identity.parent)) {
                valid = false;
                return false;
            }
            Bitmap references;
            if(!bitmap.empty()) {
                auto decoded = read_bitmap(bitmap.data(), bitmap.size());
                if(!decoded) {
                    valid = false;
                    return false;
                }
                references = std::move(*decoded);
            }
            if(!references.isEmpty() && references.maximum() >= file_ids_map.size()) {
                valid = false;
                return false;
            }
            if(identity.file != no_file && identity.file >= file_ids_map.size()) {
                valid = false;
                return false;
            }
            staged.push_back({hash, identity, std::move(references)});
            return true;
        });
    if(!valid) {
        return false;
    }

    // Units may spell one symbol differently (`X<int>` against
    // `X<signed int>`, a conversion to a typedef): the shortest spelling
    // wins, then the smaller one, so the table reads the same whatever the
    // merge order.
    auto prefer = [](std::string& current, llvm::StringRef incoming) {
        if(incoming.empty() ||
           (!current.empty() && (incoming.size() > current.size() ||
                                 (incoming.size() == current.size() && incoming >= current)))) {
            return false;
        }
        current = incoming.str();
        return true;
    };
    for(auto& [hash, identity, references]: staged) {
        bool known = identity_of(hash).has_value();
        auto& target = touch(hash);
        bool changed_row = !known;
        if(target.name.empty() && !identity.name.empty()) {
            target.parent = identity.parent;
            target.kind = identity.kind;
            changed_row = true;
        }
        changed_row = prefer(target.name, identity.name) || changed_row;
        changed_row = prefer(target.args, identity.args) || changed_row;
        // A unit that defines the symbol always places it: the table never
        // retracts a unit's earlier report, so an old definition bit must
        // not pin the file after the definition moved. Declarations only
        // fill an empty slot.
        bool defines = has_flag(identity.flags, SymbolFlags::HasDefinition);
        if(identity.file != no_file && (target.file == no_file || defines)) {
            auto file = file_ids_map[identity.file].raw;
            changed_row = changed_row || target.file != file;
            target.file = file;
        }
        auto flags = target.flags | identity.flags;
        changed_row = changed_row || flags != target.flags;
        target.flags = flags;
        for(auto ref: references) {
            target.reference_files.add(file_ids_map[ref].raw);
        }
        if(changed_row && added) {
            added->push_back(hash);
        }
    }

    return true;
}

void ProjectIndex::serialize_global(llvm::raw_ostream& os, const FileTable& files) {
    // The rows changed since the last write join the ones still held
    // aside from a write that never landed; the newer row wins.
    for(auto& [hash, row]: changed) {
        written[hash] = std::move(row);
    }
    changed.clear();

    // The blob carries only the versions some manifest still references —
    // the persisted form's garbage collection. The shared table itself is
    // left alone: other consumers may still anchor checks on a version the
    // index dropped, and ids are never reused either way.
    llvm::DenseSet<VersionID> referenced;
    for(auto& manifest: llvm::make_second_range(manifests)) {
        referenced.insert(manifest.tu_fv);
        for(auto& node: manifest.nodes) {
            referenced.insert(VersionID{node.file});
        }
        for(auto& [fv, hash]: manifest.contributions) {
            referenced.insert(fv);
        }
        referenced.insert(manifest.absent.begin(), manifest.absent.end());
    }

    GlobalBlob blob;
    blob.format_version = index_format_version;
    blob.generation = global_generation;

    llvm::SmallVector<std::pair<std::uint32_t, VersionID>> ids;
    ids.reserve(referenced.size());
    for(auto id: referenced) {
        ids.emplace_back(persisted_id(id), id);
    }
    llvm::sort(ids);
    for(auto [persisted, id]: ids) {
        auto& record = files.version(id);
        blob.fv_ids.push_back(persisted);
        blob.fv_paths.push_back(portable(files.resolve(record.fid)));
        blob.fv_hashes.push_back(record.content_hash);
    }
    blob.next_fv_id = next_persisted_id;

    blob.manifest_fvs.reserve(manifests.size());
    blob.manifest_gens.reserve(manifests.size());
    for(auto& manifest: llvm::make_second_range(manifests)) {
        blob.manifest_fvs.push_back(persisted_ids.find(manifest.tu_fv)->second);
        blob.manifest_gens.push_back(manifest.global_gen);
    }

    // The base's path table stays as it is, so its rows' bitmap images
    // copy verbatim; files the written rows reference beyond it are
    // appended.
    llvm::DenseMap<Fid, std::uint32_t> id_of;
    if(base) {
        blob.paths.reserve(base->paths.size());
        for(std::uint32_t i = 0; i < base->paths.size(); i += 1) {
            blob.paths.emplace_back(to_ref(base->paths[i]));
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

    auto count = (base ? base->count() : 0) + written.size();
    blob.sym_hashes.reserve(count);
    blob.sym_name_ends.reserve(count);
    blob.sym_args_ends.reserve(count);
    blob.sym_parents.reserve(count);
    blob.sym_kinds.reserve(count);
    blob.sym_flags.reserve(count);
    blob.sym_files.reserve(count);
    blob.sym_reference_counts.reserve(count);
    blob.sym_bitmap_ends.reserve(count);
    auto emit_base = [&](std::uint32_t doc) {
        blob.sym_hashes.push_back(base->hashes[doc]);
        blob.sym_names += base->name(doc);
        blob.sym_name_ends.push_back(static_cast<std::uint32_t>(blob.sym_names.size()));
        blob.sym_args += base->arguments(doc);
        blob.sym_args_ends.push_back(static_cast<std::uint32_t>(blob.sym_args.size()));
        blob.sym_parents.push_back(base->parents[doc]);
        blob.sym_kinds.push_back(base->kinds[doc]);
        blob.sym_flags.push_back(base->flags[doc]);
        blob.sym_files.push_back(base->files[doc]);
        blob.sym_reference_counts.push_back(base->reference_counts[doc]);
        auto image = base->image(doc);
        blob.sym_bitmaps.insert(blob.sym_bitmaps.end(), image.begin(), image.end());
        blob.sym_bitmap_ends.push_back(static_cast<std::uint32_t>(blob.sym_bitmaps.size()));
    };
    auto emit_row = [&](SymbolHash hash, const Symbol& row) {
        blob.sym_hashes.push_back(hash);
        blob.sym_names += row.name;
        blob.sym_name_ends.push_back(static_cast<std::uint32_t>(blob.sym_names.size()));
        blob.sym_args += row.args;
        blob.sym_args_ends.push_back(static_cast<std::uint32_t>(blob.sym_args.size()));
        blob.sym_parents.push_back(row.parent);
        blob.sym_kinds.push_back(row.kind.value());
        blob.sym_flags.push_back(static_cast<std::uint16_t>(row.flags));
        blob.sym_files.push_back(row.file == no_file ? no_file : path_id(Fid{row.file}));
        blob.sym_reference_counts.push_back(
            static_cast<std::uint32_t>(row.reference_files.cardinality()));
        Bitmap references;
        for(auto id: row.reference_files) {
            references.add(path_id(Fid{id}));
        }
        auto image = write_bitmap(references);
        auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
        blob.sym_bitmaps.insert(blob.sym_bitmaps.end(), bytes, bytes + image.size());
        blob.sym_bitmap_ends.push_back(static_cast<std::uint32_t>(blob.sym_bitmaps.size()));
    };

    llvm::SmallVector<SymbolHash> keys;
    keys.reserve(written.size());
    for(auto hash: llvm::make_first_range(written)) {
        keys.push_back(hash);
    }
    llvm::sort(keys);
    std::uint32_t doc = 0;
    std::size_t k = 0;
    auto base_count = base ? base->count() : 0;
    while(doc < base_count || k < keys.size()) {
        if(k == keys.size() || (doc < base_count && base->hashes[doc] < keys[k])) {
            emit_base(doc);
            doc += 1;
            continue;
        }
        if(doc < base_count && base->hashes[doc] == keys[k]) {
            doc += 1;
        }
        emit_row(keys[k], written.find(keys[k])->second);
        k += 1;
    }

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
        auto image = write_bitmap(tus);
        auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
        blob.contributors.insert(blob.contributors.end(), bytes, bytes + image.size());
        blob.contributor_ends.push_back(static_cast<std::uint32_t>(blob.contributors.size()));
    }

    blob.search_generation = search_generation;
    blob.search_pending.assign(search_pending.begin(), search_pending.end());
    llvm::sort(blob.search_pending);

    serialize_blob(blob, os);
}

void ProjectIndex::rebase(std::unique_ptr<llvm::MemoryBuffer> blob, FileTable& files) {
    [[maybe_unused]] bool bound = bind_global(std::move(blob), files);
    assert(bound && "a freshly serialized global blob must bind");
    written.clear();
}

void ProjectIndex::restore_unwritten() {
    for(auto& [hash, row]: written) {
        changed.try_emplace(hash, std::move(row));
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
    auto import = [&](std::uint32_t& id) {
        auto version = runtime_version(id);
        if(!version) {
            return false;
        }
        id = version->raw;
        return true;
    };
    if(!import(manifest.tu_fv.raw)) {
        return false;
    }
    for(auto& node: manifest.nodes) {
        if(!import(node.file)) {
            return false;
        }
    }
    for(auto& fv: llvm::make_first_range(manifest.contributions)) {
        if(!import(fv.raw)) {
            return false;
        }
    }
    for(auto& fv: manifest.absent) {
        if(!import(fv.raw)) {
            return false;
        }
    }
    return true;
}

TUManifest ProjectIndex::export_manifest(const TUManifest& manifest) {
    auto exported = manifest;
    exported.tu_fv.raw = persisted_id(manifest.tu_fv);
    for(auto& node: exported.nodes) {
        node.file = persisted_id(VersionID{node.file});
    }
    for(auto& fv: llvm::make_first_range(exported.contributions)) {
        fv.raw = persisted_id(fv);
    }
    for(auto& fv: exported.absent) {
        fv.raw = persisted_id(fv);
    }
    return exported;
}

std::uint32_t ProjectIndex::persisted_id(VersionID version) {
    auto [it, inserted] = persisted_ids.try_emplace(version, next_persisted_id);
    if(inserted) {
        next_persisted_id += 1;
    }
    return it->second;
}

llvm::SmallVector<Fid> ProjectIndex::apply_manifest(const FileTable& files,
                                                    Fid tu_path_id,
                                                    TUManifest manifest) {
    llvm::SmallVector<Fid> affected = remove_manifest(files, tu_path_id);

    for(auto& [fv, hash]: manifest.contributions) {
        auto path_id = files.version(fv).fid;
        contributions[path_id][tu_path_id] = hash;
        affected.push_back(path_id);
    }
    // Two spellings of one place, or two persisted versions of it, are one
    // file: remove_manifest erases each place once.
    llvm::sort(manifest.absent);
    manifest.absent.erase(llvm::unique(manifest.absent), manifest.absent.end());
    for(auto fv: manifest.absent) {
        probed[files.version(fv).fid].insert(tu_path_id);
    }
    manifests[tu_path_id] = std::move(manifest);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    return affected;
}

llvm::SmallVector<Fid> ProjectIndex::remove_manifest(const FileTable& files, Fid tu_path_id) {
    llvm::SmallVector<Fid> affected;
    auto it = manifests.find(tu_path_id);
    if(it == manifests.end()) {
        return affected;
    }

    for(auto& [fv, hash]: it->second.contributions) {
        auto path_id = files.version(fv).fid;
        auto contribution_it = contributions.find(path_id);
        if(contribution_it == contributions.end()) {
            continue;
        }
        contribution_it->second.erase(tu_path_id);
        if(contribution_it->second.empty()) {
            contributions.erase(contribution_it);
        }
        affected.push_back(path_id);
    }
    for(auto fv: it->second.absent) {
        auto probe_it = probed.find(files.version(fv).fid);
        probe_it->second.erase(tu_path_id);
        if(probe_it->second.empty()) {
            probed.erase(probe_it);
        }
    }
    manifests.erase(it);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    return affected;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(Fid path_id) const {
    llvm::SmallVector<std::uint64_t> variants;
    auto it = contributions.find(path_id);
    if(it == contributions.end()) {
        return variants;
    }
    for(auto hash: llvm::make_second_range(it->second)) {
        variants.push_back(hash);
    }
    llvm::sort(variants);
    variants.erase(llvm::unique(variants), variants.end());
    return variants;
}

void ProjectIndex::each_contributor(Fid file, llvm::function_ref<void(Fid)> visit) const {
    if(!db) {
        if(auto it = contributions.find(file); it != contributions.end()) {
            for(auto tu: llvm::make_first_range(it->second)) {
                visit(tu);
            }
        }
        return;
    }
    if(!base) {
        return;
    }
    auto tus = base->contributors_of(file);
    if(!tus) {
        return;
    }
    for(auto id: *tus) {
        if(id < base->remap.size()) {
            visit(base->remap[id]);
        }
    }
}

void ProjectIndex::each_fanout_file(SymbolHash hash,
                                    Fid anchor,
                                    const FileTable& files,
                                    llvm::function_ref<void(Fid)> visit) const {
    llvm::SmallDenseSet<Fid, 8> seen{anchor};
    llvm::SmallDenseSet<Fid, 8> asked;
    llvm::SmallVector<Fid> pending{anchor};
    visit(anchor);
    while(!pending.empty()) {
        each_contributor(pending.pop_back_val(), [&](Fid tu) {
            auto* manifest = asked.insert(tu).second ? tu_manifest(tu) : nullptr;
            if(!manifest) {
                return;
            }
            auto it =
                std::ranges::lower_bound(manifest->local_fanout, hash, {}, &LocalFanout::symbol);
            if(it == manifest->local_fanout.end() || it->symbol != hash) {
                return;
            }
            for(auto index: it->files) {
                auto version = manifest->contributions[index].first;
                std::optional<Fid> file;
                if(!db) {
                    file = files.version(version).fid;
                } else if(auto name = base->version_path(version.raw)) {
                    file = files.find(local(*name));
                }
                if(file && seen.insert(*file).second) {
                    visit(*file);
                    pending.push_back(*file);
                }
            }
        });
    }
}

const TUManifest* ProjectIndex::tu_manifest(Fid tu) const {
    if(!db) {
        auto it = manifests.find(tu);
        return it != manifests.end() ? &it->second : nullptr;
    }
    auto [it, inserted] = fetched_manifests.try_emplace(tu);
    if(inserted) {
        // A reader adopts a manifest only under the stamp the bound global
        // pins for it, as the writer's load does; the versions it names
        // stay persisted ids.
        auto pinned = [&](const TUManifest& manifest) {
            auto pin = std::ranges::find(base->manifest_fvs, manifest.tu_fv.raw);
            return pin != base->manifest_fvs.end() &&
                   base->manifest_gens[pin - base->manifest_fvs.begin()] == manifest.global_gen;
        };
        if(auto blob = db->read(IndexBlobKind::Manifest, key_of(*files, tu))) {
            if(auto manifest = deserialize_manifest(blob.buffer->getBuffer());
               manifest && pinned(*manifest)) {
                it->second = std::move(*manifest);
            }
        }
    }
    return it->second ? &*it->second : nullptr;
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
    if(auto blob = db->read(IndexBlobKind::Shard, key_of(*files, file))) {
        slot = Shard::from_buffer(std::move(blob.buffer));
    }
    return slot.loaded() ? &slot : nullptr;
}

bool ProjectIndex::bind_search(std::unique_ptr<llvm::MemoryBuffer> blob) {
    SearchIndex built;
    if(!built.load(std::move(blob)) || built.generation() != search_generation) {
        return false;
    }
    search_index = std::move(built);
    return true;
}

bool ProjectIndex::open(BlobDatabase& db, FileTable& files) {
    auto global = db.read(IndexBlobKind::Global, "global");
    if(!global || !bind_global(std::move(global.buffer), files)) {
        return false;
    }
    global_generation = base->generation;
    search_generation = base->search_generation;
    search_pending.clear();
    search_pending.insert(base->search_pending.begin(), base->search_pending.end());
    if(auto search = db.read(IndexBlobKind::Search, "search")) {
        bind_search(std::move(search.buffer));
    }
    this->db = &db;
    this->files = &files;
    return true;
}

ProjectIndex::GlobalColumns ProjectIndex::global_columns() const {
    if(!base) {
        return {};
    }
    return {
        .names = base->names.size(),
        .args = base->args.size(),
        .bitmaps = base->bitmaps.size(),
        .fixed = base->count() * (8 + 8 + 1 + 2 + 4 + 4),
        .contributors = base->contributors.size() + base->contributed_files.size() * (4 + 4),
    };
}

}  // namespace clice::index
