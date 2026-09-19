#include "index/project_index.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

#include "index/serialization.h"
#include "support/logging.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

namespace clice::index {

namespace {

/// Upper bound on the FileVersion id space a blob may claim, rejecting
/// corrupt counters before the id-space resize tries to honor them
/// (versions are restored id-for-id into a table sized by this counter,
/// so a garbage high-water mark would allocate gigabytes before anything
/// could reject it). The cap is far beyond any table a writer accumulates
/// (ids grow only with newly seen (file, content-hash) pairs), and a
/// table that ever drifts there is better compacted by the rebuild the
/// rejection triggers.
constexpr inline std::uint32_t max_persisted_versions = 1u << 24;

/// The global layer's persisted form, read in place: the FileVersion table
/// as parallel columns, the symbol table as parallel columns in hash order
/// with its names, arguments and reference bitmaps in arenas, and the path
/// table those columns index.
struct GlobalBlob {
    std::uint32_t format_version = 0;

    /// See ProjectIndex::global_generation.
    std::uint64_t generation = 0;

    /// See FileTable::revocation_generation.
    std::uint64_t revocation_generation = 0;

    std::uint32_t next_fv_id = 0;

    std::vector<std::uint32_t> fv_ids;
    std::vector<std::string> fv_paths;
    std::vector<std::uint64_t> fv_hashes;
    std::vector<std::uint64_t> fv_sizes;
    std::vector<std::int64_t> fv_mtimes;

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
    std::uint64_t revocation_generation = 0;
    std::uint32_t next_fv_id = 0;
    llvm::ArrayRef<std::uint32_t> fv_ids;
    kota::codec::fbs::array_view<std::string> fv_paths;
    llvm::ArrayRef<std::uint64_t> fv_hashes;
    llvm::ArrayRef<std::uint64_t> fv_sizes;
    llvm::ArrayRef<std::int64_t> fv_mtimes;

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

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

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

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t doc) const {
        auto begin = doc == 0 ? 0 : bitmap_ends[doc - 1];
        return bitmaps.slice(begin, bitmap_ends[doc] - begin);
    }

    std::optional<Bitmap> bitmap(std::uint32_t doc) const {
        auto bytes = image(doc);
        return view_bitmap(bytes.data(), bytes.size());
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
    revocation_generation = root[&GlobalBlob::revocation_generation];
    next_fv_id = root[&GlobalBlob::next_fv_id];
    fv_ids = to_array_ref(root[&GlobalBlob::fv_ids]);
    fv_paths = root[&GlobalBlob::fv_paths];
    fv_hashes = to_array_ref(root[&GlobalBlob::fv_hashes]);
    fv_sizes = to_array_ref(root[&GlobalBlob::fv_sizes]);
    fv_mtimes = to_array_ref(root[&GlobalBlob::fv_mtimes]);
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

    auto version_count = fv_ids.size();
    if(fv_paths.size() != version_count || fv_hashes.size() != version_count ||
       fv_sizes.size() != version_count || fv_mtimes.size() != version_count) {
        return std::unexpected("file version columns do not line up");
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
    // Hashes and parents become table keys downstream, which reserve two
    // sentinel values; a repeated hash would let one row shadow another.
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
        bound->remap.push_back(files.intern(to_ref(bound->paths[i])));
    }
    bound->buffer = std::move(blob);
    base = std::move(bound);
    return true;
}

std::expected<void, llvm::StringRef> ProjectIndex::adopt_file_versions(
    FileTable& files,
    llvm::DenseMap<VersionID, std::uint64_t>& manifest_pins) const {
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
    // duplicate checks here and then in the tables themselves — see
    // reserved_key. The counter is one intern away from becoming a key
    // itself, and the writer hands ids out from it, so ids must sit below
    // it — a bound that (with the sentinels at the top of the id space)
    // also keeps every id non-reserved.
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
    // would leave fv_ids interning the earlier pair to an id whose record
    // names the later path, attributing contributions to the wrong file.
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

    // Adopting the blob's version ids verbatim is what keeps persisted
    // manifests resolvable; it requires an untouched table — ids already
    // handed out by this session could collide with the blob's. Ids the
    // writer garbage-collected stay behind as holes (see knows_version).
    assert(files.versions.empty() && "the global blob must load before any version interning");
    files.revocation_generation = blob.revocation_generation;
    files.versions.resize(blob.next_fv_id);
    for(std::size_t i = 0; i < count; i += 1) {
        auto path_id = files.intern(to_ref(blob.fv_paths[i]));
        auto id = VersionID{blob.fv_ids[i]};
        files.versions[id.raw] =
            FileTable::FileVersion{path_id, blob.fv_hashes[i], blob.fv_sizes[i], blob.fv_mtimes[i]};
        files.version_ids[{path_id, blob.fv_hashes[i]}] = id;
    }
    for(std::size_t k = 0; k < blob.manifest_fvs.size(); k += 1) {
        manifest_pins[VersionID{blob.manifest_fvs[k]}] = blob.manifest_gens[k];
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
    }

    GlobalBlob blob;
    blob.format_version = index_format_version;
    blob.generation = global_generation;
    blob.revocation_generation = files.revocation_generation;
    blob.next_fv_id = static_cast<std::uint32_t>(files.versions.size());

    llvm::SmallVector<VersionID> ids(referenced.begin(), referenced.end());
    llvm::sort(ids);
    for(auto id: ids) {
        auto& record = files.version(id);
        blob.fv_ids.push_back(id.raw);
        blob.fv_paths.emplace_back(files.resolve(record.fid));
        blob.fv_hashes.push_back(record.content_hash);
        blob.fv_sizes.push_back(record.size);
        blob.fv_mtimes.push_back(record.mtime_ns);
    }

    blob.manifest_fvs.reserve(manifests.size());
    blob.manifest_gens.reserve(manifests.size());
    for(auto& manifest: llvm::make_second_range(manifests)) {
        blob.manifest_fvs.push_back(manifest.tu_fv.raw);
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
            blob.paths.emplace_back(files.resolve(file));
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

bool ProjectIndex::knows_file_versions(const FileTable& files, const TUManifest& manifest) const {
    if(!files.knows_version(manifest.tu_fv)) {
        return false;
    }
    for(auto& node: manifest.nodes) {
        if(!files.knows_version(VersionID{node.file})) {
            return false;
        }
    }
    for(auto& [fv, hash]: manifest.contributions) {
        if(!files.knows_version(fv)) {
            return false;
        }
    }
    return true;
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
    if(auto blob = db->read(IndexBlobKind::Shard, blob_key(files->resolve(file)))) {
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
    };
}

}  // namespace clice::index
