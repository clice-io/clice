module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.instantiation_shard;
import :index.project_index;
import :index.serialization;
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
    /// long as the blob lineage lives, so a base entry's bitmap image is
    /// copied into the next blob verbatim.
    std::vector<std::string> paths;

    /// The external symbol table, sorted by hash. A symbol's reference
    /// files are a file set (append_file_set) in the arena, one end offset
    /// per symbol.
    std::vector<std::uint64_t> sym_hashes;
    std::string sym_names;
    std::vector<std::uint32_t> sym_name_ends;
    /// The arguments of the few symbols having them: those symbols,
    /// ascending, and their text back to back.
    std::vector<std::uint32_t> sym_args_docs;
    std::string sym_args;
    std::vector<std::uint32_t> sym_args_ends;
    std::string sym_signatures;
    std::vector<std::uint32_t> sym_signature_ends;
    /// The parent's position in the table plus one; 0 for none, or for one
    /// outside the table, which the sparse (symbol, parent hash) table names.
    IdColumn sym_parents;
    std::vector<std::uint32_t> sym_far_parents;
    std::vector<std::uint64_t> sym_far_parent_hashes;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::uint16_t> sym_flags;
    std::vector<std::uint8_t> sym_token_kinds;
    std::vector<std::uint32_t> sym_token_modifiers;
    /// Path ids plus one, 0 for a symbol without a declaring relation.
    IdColumn sym_files;
    /// The files referencing each symbol, counted: at most the path count,
    /// so they take the width of path ids.
    IdColumn sym_reference_counts;
    std::vector<std::uint32_t> sym_bitmap_ends;
    std::vector<std::uint8_t> sym_bitmaps;
    /// The files whose instantiation shards name a symbol, as file sets for
    /// the few symbols some instantiation names, ascending.
    std::vector<std::uint32_t> sym_instantiation_docs;
    std::vector<std::uint32_t> sym_instantiation_ends;
    std::vector<std::uint8_t> sym_instantiations;

    /// tu_fv -> generation of every manifest current at this save.
    /// The loader adopts a manifest blob only on an exact generation match, so
    /// a manifest whose write failed while the global was persisted (or one that
    /// outran a lost global write) reads as lost and its TU reindexes,
    /// instead of an older on-disk manifest serving as current.
    std::vector<std::uint32_t> manifest_fvs;
    std::vector<std::uint64_t> manifest_gens;

    /// The generation of the search blob this table was saved with, and
    /// the entries and the files whose names of internal linkage (path
    /// ids, ascending) changed since that blob was built.
    std::uint64_t search_generation = 0;
    std::vector<std::uint64_t> search_pending;
    std::vector<std::uint32_t> search_pending_files;

    /// The reverse include graph, for readers holding no manifests: every
    /// file some TU contributed a variant to (path ids, ascending), and those
    /// TUs' path ids as file sets back to back.
    std::vector<std::uint32_t> contributed_files;
    std::vector<std::uint32_t> contributor_ends;
    std::vector<std::uint8_t> contributors;

    /// The units whose local fanout names a file (TUManifest::local_fanout),
    /// laid out like the contributors: what a reader walks from a file.
    std::vector<std::uint32_t> fanout_files;
    std::vector<std::uint32_t> fanout_unit_ends;
    std::vector<std::uint8_t> fanout_units;
};

using BlobView = kota::codec::fbs::table_view<GlobalBlob>;

/// Most file sets hold one or two files: up to raw_file_set_limit path ids
/// are stored raw as u32s, more as a portable roaring image — which is
/// longer than any raw list, so a slice's length says which it is.
constexpr std::size_t raw_file_set_limit = 2;

void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids) {
    if(ids.cardinality() <= raw_file_set_limit) {
        for(std::uint32_t id: ids) {
            auto* bytes = reinterpret_cast<const std::uint8_t*>(&id);
            arena.insert(arena.end(), bytes, bytes + sizeof(id));
        }
        return;
    }
    auto image = write_bitmap(ids);
    auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
    arena.insert(arena.end(), bytes, bytes + image.size());
}

/// The set a slice holds; nullopt when it is not one append_file_set writes.
std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes) {
    if(bytes.size() > raw_file_set_limit * sizeof(std::uint32_t)) {
        auto image = view_bitmap(bytes.data(), bytes.size());
        if(!image || image->cardinality() <= raw_file_set_limit) {
            return std::nullopt;
        }
        return image;
    }
    if(bytes.size() % sizeof(std::uint32_t) != 0) {
        return std::nullopt;
    }
    Bitmap ids;
    std::optional<std::uint32_t> previous;
    for(std::size_t i = 0; i < bytes.size(); i += sizeof(std::uint32_t)) {
        std::uint32_t id;
        std::memcpy(&id, bytes.data() + i, sizeof(id));
        if(previous && id <= *previous) {
            return std::nullopt;
        }
        ids.add(id);
        previous = id;
    }
    return ids;
}

/// A table derived from the manifests: file -> (TU -> the hash it names there).
using UnitHashes = llvm::DenseMap<Fid, llvm::SmallDenseMap<Fid, std::uint64_t, 2>>;

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

/// A persisted map from files to units, GlobalBlob's contributors and
/// fanout units: the files as path ids, ascending, and each one's units as
/// a file set of path ids, back to back.
struct FileUnits {
    llvm::ArrayRef<std::uint32_t> files;
    llvm::ArrayRef<std::uint32_t> ends;
    llvm::ArrayRef<std::uint8_t> sets;

    /// File table id -> index into `files`, built on first use.
    mutable llvm::DenseMap<Fid, std::uint32_t> index;

    bool lines_up(std::size_t path_count) const {
        if(ends.size() != files.size() || !monotone_ends(ends, sets.size())) {
            return false;
        }
        for(std::size_t i = 0; i < files.size(); i += 1) {
            if(files[i] >= path_count || (i > 0 && files[i] <= files[i - 1])) {
                return false;
            }
        }
        return true;
    }

    /// The units of `file` as path ids; nullopt when it has none or the set
    /// fails to decode.
    std::optional<Bitmap> of(Fid file, llvm::ArrayRef<Fid> remap) const {
        if(index.empty()) {
            for(std::uint32_t i = 0; i < files.size(); i += 1) {
                index.try_emplace(remap[files[i]], i);
            }
        }
        auto it = index.find(file);
        if(it == index.end()) {
            return std::nullopt;
        }
        return read_file_set(back_to_back(sets, ends, it->second));
    }
};

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
    llvm::ArrayRef<std::uint32_t> args_docs;
    llvm::StringRef args;
    llvm::ArrayRef<std::uint32_t> args_ends;
    llvm::StringRef signatures;
    llvm::ArrayRef<std::uint32_t> signature_ends;
    IdColumnView parents;
    llvm::ArrayRef<std::uint32_t> far_parents;
    llvm::ArrayRef<std::uint64_t> far_parent_hashes;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint8_t> token_kinds;
    llvm::ArrayRef<std::uint32_t> token_modifiers;
    IdColumnView files;
    IdColumnView reference_counts;
    llvm::ArrayRef<std::uint32_t> bitmap_ends;
    llvm::ArrayRef<std::uint8_t> bitmaps;
    llvm::ArrayRef<std::uint32_t> instantiation_docs;
    llvm::ArrayRef<std::uint32_t> instantiation_ends;
    llvm::ArrayRef<std::uint8_t> instantiation_images;

    llvm::ArrayRef<std::uint32_t> manifest_fvs;
    llvm::ArrayRef<std::uint64_t> manifest_gens;

    std::uint64_t search_generation = 0;
    llvm::ArrayRef<std::uint64_t> search_pending;
    llvm::ArrayRef<std::uint32_t> search_pending_files;

    FileUnits contributors;
    FileUnits fanout_units;

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

    /// Visit the units `table` lists for `file`.
    void each_unit(const FileUnits& table, Fid file, llvm::function_ref<void(Fid)> visit) const {
        auto tus = table.of(file, remap);
        if(!tus) {
            return;
        }
        for(auto id: *tus) {
            if(id < remap.size()) {
                visit(remap[id]);
            }
        }
    }

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
        return back_to_back(names, name_ends, doc);
    }

    llvm::StringRef arguments(std::uint32_t doc) const {
        auto it = std::ranges::lower_bound(args_docs, doc);
        if(it == args_docs.end() || *it != doc) {
            return {};
        }
        return back_to_back(args, args_ends, static_cast<std::uint32_t>(it - args_docs.begin()));
    }

    SymbolHash parent(std::uint32_t doc) const {
        if(parents[doc] != 0) {
            return hashes[parents[doc] - 1];
        }
        auto it = std::ranges::lower_bound(far_parents, doc);
        if(it == far_parents.end() || *it != doc) {
            return 0;
        }
        return far_parent_hashes[it - far_parents.begin()];
    }

    /// The symbol's file as a path id of the blob, or no_file.
    std::uint32_t file(std::uint32_t doc) const {
        return files[doc] == 0 ? no_file : files[doc] - 1;
    }

    llvm::StringRef signature(std::uint32_t doc) const {
        return back_to_back(signatures, signature_ends, doc);
    }

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t doc) const {
        return back_to_back(bitmaps, bitmap_ends, doc);
    }

    std::optional<Bitmap> bitmap(std::uint32_t doc) const {
        return read_file_set(image(doc));
    }

    /// Empty for a symbol no instantiation names.
    llvm::ArrayRef<std::uint8_t> instantiation_image(std::uint32_t doc) const {
        auto it = std::ranges::lower_bound(instantiation_docs, doc);
        if(it == instantiation_docs.end() || *it != doc) {
            return {};
        }
        return back_to_back(instantiation_images,
                            instantiation_ends,
                            static_cast<std::uint32_t>(it - instantiation_docs.begin()));
    }

    std::optional<Bitmap> instantiation_bitmap(std::uint32_t doc) const {
        return read_file_set(instantiation_image(doc));
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
        auto file = this->file(doc);
        return {
            .name = name(doc),
            .args = arguments(doc),
            .signature = signature(doc),
            .parent = parent(doc),
            .kind = SymbolKind(kinds[doc]),
            .scope = SymbolScope::External,
            .flags = static_cast<SymbolFlags>(flags[doc]),
            .file = file == no_file ? no_file : remap[file].raw,
            .token_class = {.kind = static_cast<SemanticTokenKind>(token_kinds[doc]),
                            .modifiers = token_modifiers[doc]},
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
    args_docs = to_array_ref(root[&GlobalBlob::sym_args_docs]);
    args = to_ref(root[&GlobalBlob::sym_args]);
    args_ends = to_array_ref(root[&GlobalBlob::sym_args_ends]);
    signatures = to_ref(root[&GlobalBlob::sym_signatures]);
    signature_ends = to_array_ref(root[&GlobalBlob::sym_signature_ends]);
    parents = ids_of(root[&GlobalBlob::sym_parents], hashes.size() + 1);
    far_parents = to_array_ref(root[&GlobalBlob::sym_far_parents]);
    far_parent_hashes = to_array_ref(root[&GlobalBlob::sym_far_parent_hashes]);
    kinds = to_array_ref(root[&GlobalBlob::sym_kinds]);
    flags = to_array_ref(root[&GlobalBlob::sym_flags]);
    token_kinds = to_array_ref(root[&GlobalBlob::sym_token_kinds]);
    token_modifiers = to_array_ref(root[&GlobalBlob::sym_token_modifiers]);
    files = ids_of(root[&GlobalBlob::sym_files], paths.size() + 1);
    reference_counts = ids_of(root[&GlobalBlob::sym_reference_counts], paths.size() + 1);
    bitmap_ends = to_array_ref(root[&GlobalBlob::sym_bitmap_ends]);
    bitmaps = to_array_ref(root[&GlobalBlob::sym_bitmaps]);
    instantiation_docs = to_array_ref(root[&GlobalBlob::sym_instantiation_docs]);
    instantiation_ends = to_array_ref(root[&GlobalBlob::sym_instantiation_ends]);
    instantiation_images = to_array_ref(root[&GlobalBlob::sym_instantiations]);
    manifest_fvs = to_array_ref(root[&GlobalBlob::manifest_fvs]);
    manifest_gens = to_array_ref(root[&GlobalBlob::manifest_gens]);
    search_generation = root[&GlobalBlob::search_generation];
    search_pending = to_array_ref(root[&GlobalBlob::search_pending]);
    search_pending_files = to_array_ref(root[&GlobalBlob::search_pending_files]);
    contributors.files = to_array_ref(root[&GlobalBlob::contributed_files]);
    contributors.ends = to_array_ref(root[&GlobalBlob::contributor_ends]);
    contributors.sets = to_array_ref(root[&GlobalBlob::contributors]);
    fanout_units.files = to_array_ref(root[&GlobalBlob::fanout_files]);
    fanout_units.ends = to_array_ref(root[&GlobalBlob::fanout_unit_ends]);
    fanout_units.sets = to_array_ref(root[&GlobalBlob::fanout_units]);

    auto version_count = fv_ids.size();
    if(fv_paths.size() != version_count || fv_hashes.size() != version_count) {
        return std::unexpected("file version columns do not line up");
    }
    if(!std::ranges::is_sorted(fv_ids)) {
        return std::unexpected("file version ids are not ascending");
    }
    if(!contributors.lines_up(paths.size())) {
        return std::unexpected("contributor columns do not line up");
    }
    if(!fanout_units.lines_up(paths.size())) {
        return std::unexpected("fanout unit columns do not line up");
    }
    if(manifest_fvs.size() != manifest_gens.size()) {
        return std::unexpected("manifest pin columns do not line up");
    }
    auto count = hashes.size();
    if(name_ends.size() != count || signature_ends.size() != count || kinds.size() != count ||
       flags.size() != count || token_kinds.size() != count || token_modifiers.size() != count ||
       bitmap_ends.size() != count) {
        return std::unexpected("symbol columns do not line up");
    }
    if(!all_token_kinds(token_kinds)) {
        return std::unexpected("a symbol's token kind is out of range");
    }
    if(!ids_ok(parents, count, count + 1) || !ids_ok(files, count, paths.size() + 1) ||
       !ids_ok(reference_counts, count, paths.size() + 1)) {
        return std::unexpected("symbol id columns do not line up");
    }
    auto sparse = [&](llvm::ArrayRef<std::uint32_t> docs, std::size_t values) {
        return sparse_ok(docs, values, count);
    };
    if(!sparse(args_docs, args_ends.size()) ||
       !sparse(instantiation_docs, instantiation_ends.size()) ||
       !sparse(far_parents, far_parent_hashes.size())) {
        return std::unexpected("sparse symbol columns do not line up");
    }
    if(!monotone_ends(name_ends, names.size()) || !monotone_ends(args_ends, args.size()) ||
       !monotone_ends(signature_ends, signatures.size()) ||
       !monotone_ends(bitmap_ends, bitmaps.size()) ||
       !monotone_ends(instantiation_ends, instantiation_images.size())) {
        return std::unexpected("symbol arenas do not line up");
    }
    // Hashes, parents and pending entries become table keys downstream, which
    // reserve two sentinel values; a repeated hash would let one entry shadow
    // another.
    for(std::size_t i = 0; i < count; i += 1) {
        if(reserved_key(hashes[i]) || (i > 0 && hashes[i] <= hashes[i - 1])) {
            return std::unexpected("symbol hashes are not ascending");
        }
    }
    for(std::size_t k = 0; k < far_parents.size(); k += 1) {
        if(parents[far_parents[k]] != 0 || reserved_key(far_parent_hashes[k])) {
            return std::unexpected("symbol parent outside the table is not canonical");
        }
    }
    for(auto hash: search_pending) {
        if(reserved_key(hash)) {
            return std::unexpected("reserved pending search row");
        }
    }
    for(auto file: search_pending_files) {
        if(file >= paths.size()) {
            return std::unexpected("pending search file outside the path table");
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
    auto in_paths = [&](const std::optional<Bitmap>& decoded) {
        return decoded && (decoded->isEmpty() || decoded->maximum() < base->paths.size());
    };
    for(std::uint32_t doc = 0; doc < base->count(); doc += 1) {
        if(!in_paths(base->bitmap(doc)) || !in_paths(base->instantiation_bitmap(doc))) {
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
    search_pending_files.clear();
    for(auto file: base->search_pending_files) {
        search_pending_files.insert(base->remap[file]);
    }
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
    auto own = [&](const Symbol& symbol) {
        for(auto id: symbol.reference_files) {
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

void ProjectIndex::each_instantiation_file(SymbolHash hash,
                                           llvm::function_ref<void(Fid)> visit) const {
    auto own = [&](const Symbol& symbol) {
        for(auto id: symbol.instantiation_files) {
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
    auto decoded = base->instantiation_bitmap(*doc);
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
    for(auto& [hash, symbol]: changed) {
        if(!visit(hash,
                  symbol.identity(),
                  static_cast<std::uint32_t>(symbol.reference_files.cardinality()))) {
            return;
        }
    }
    for(auto& [hash, symbol]: written) {
        if(changed.contains(hash)) {
            continue;
        }
        if(!visit(hash,
                  symbol.identity(),
                  static_cast<std::uint32_t>(symbol.reference_files.cardinality()))) {
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
    auto& symbol = it->second;
    if(auto held = written.find(hash); held != written.end()) {
        symbol = held->second;
        return symbol;
    }
    if(!base) {
        return symbol;
    }
    auto doc = base->find(hash);
    if(!doc) {
        return symbol;
    }
    auto identity = base->identity(*doc);
    symbol.name = identity.name.str();
    symbol.args = identity.args.str();
    symbol.signature = identity.signature.str();
    symbol.parent = identity.parent;
    symbol.kind = identity.kind;
    symbol.flags = identity.flags;
    symbol.token_class = identity.token_class;
    symbol.file = identity.file;
    if(auto decoded = base->bitmap(*doc)) {
        for(auto id: *decoded) {
            if(id < base->remap.size()) {
                symbol.reference_files.add(base->remap[id].raw);
            }
        }
    }
    if(auto decoded = base->instantiation_bitmap(*doc)) {
        for(auto id: *decoded) {
            if(id < base->remap.size()) {
                symbol.instantiation_files.add(base->remap[id].raw);
            }
        }
    }
    return symbol;
}

void ProjectIndex::merge(const TUIndex& index,
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
    index.iterate_symbols([&](SymbolHash hash,
                              const SymbolIdentity& identity,
                              llvm::ArrayRef<std::uint32_t> references) {
        if(identity.scope != SymbolScope::External) {
            return true;
        }
        bool known = identity_of(hash).has_value();
        auto& target = touch(hash);
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
    index.iterate_instantiation_files([&](SymbolHash hash, llvm::ArrayRef<std::uint32_t> files) {
        auto& target = touch(hash);
        for(auto file: files) {
            target.instantiation_files.add(file_ids_map[file].raw);
        }
        return true;
    });
}

void ProjectIndex::serialize_global(llvm::raw_ostream& os, const FileTable& files) {
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
        referenced.insert(manifest.tu_fv);
        for(auto& node: manifest.nodes) {
            referenced.insert(VersionID{node.file});
        }
        for(auto& [fv, hash]: manifest.contributions) {
            referenced.insert(fv);
        }
        for(auto& use: manifest.instantiations) {
            referenced.insert(use.file);
        }
        for(auto& fanout: manifest.local_fanout) {
            referenced.insert(fanout.files.begin(), fanout.files.end());
        }
        referenced.insert(manifest.absent.begin(), manifest.absent.end());
        referenced.insert(manifest.imports.begin(), manifest.imports.end());
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

    // The base's path table stays as it is, so its entries' bitmap images
    // copy verbatim; files the written entries reference beyond it are
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

    // The table in hash order: base entries copied, written entries encoded.
    struct Entry {
        SymbolHash hash;
        std::uint32_t doc;
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
    std::uint32_t doc = 0;
    std::size_t k = 0;
    auto base_count = base ? base->count() : 0;
    while(doc < base_count || k < keys.size()) {
        if(k == keys.size() || (doc < base_count && base->hashes[doc] < keys[k])) {
            entries.push_back({base->hashes[doc], doc, nullptr});
            doc += 1;
            continue;
        }
        if(doc < base_count && base->hashes[doc] == keys[k]) {
            doc += 1;
        }
        entries.push_back({keys[k], 0, &written.find(keys[k])->second});
        k += 1;
    }
    blob.sym_hashes.reserve(entries.size());
    for(auto& entry: entries) {
        blob.sym_hashes.push_back(entry.hash);
    }

    std::vector<std::uint32_t> parents;
    std::vector<std::uint32_t> file_ids;
    std::vector<std::uint32_t> reference_counts;
    parents.reserve(entries.size());
    file_ids.reserve(entries.size());
    reference_counts.reserve(entries.size());
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        auto& [hash, doc, symbol] = entries[i];
        auto identity = symbol ? symbol->identity() : base->identity(doc);
        blob.sym_names += identity.name;
        blob.sym_name_ends.push_back(static_cast<std::uint32_t>(blob.sym_names.size()));
        if(!identity.args.empty()) {
            blob.sym_args_docs.push_back(i);
            blob.sym_args += identity.args;
            blob.sym_args_ends.push_back(static_cast<std::uint32_t>(blob.sym_args.size()));
        }
        blob.sym_signatures += identity.signature;
        blob.sym_signature_ends.push_back(static_cast<std::uint32_t>(blob.sym_signatures.size()));
        auto parent = std::ranges::lower_bound(blob.sym_hashes, identity.parent);
        if(identity.parent != 0 && parent != blob.sym_hashes.end() && *parent == identity.parent) {
            parents.push_back(static_cast<std::uint32_t>(parent - blob.sym_hashes.begin()) + 1);
        } else {
            parents.push_back(0);
            if(identity.parent != 0) {
                blob.sym_far_parents.push_back(i);
                blob.sym_far_parent_hashes.push_back(identity.parent);
            }
        }
        blob.sym_kinds.push_back(identity.kind.value());
        blob.sym_flags.push_back(static_cast<std::uint16_t>(identity.flags));
        blob.sym_token_kinds.push_back(static_cast<std::uint8_t>(identity.token_class.kind));
        blob.sym_token_modifiers.push_back(identity.token_class.modifiers);
        if(!symbol) {
            auto file = base->file(doc);
            file_ids.push_back(file == no_file ? 0 : file + 1);
            reference_counts.push_back(base->reference_counts[doc]);
            auto image = base->image(doc);
            blob.sym_bitmaps.insert(blob.sym_bitmaps.end(), image.begin(), image.end());
            blob.sym_bitmap_ends.push_back(static_cast<std::uint32_t>(blob.sym_bitmaps.size()));
            auto instantiations = base->instantiation_image(doc);
            if(!instantiations.empty()) {
                blob.sym_instantiation_docs.push_back(i);
                blob.sym_instantiations.insert(blob.sym_instantiations.end(),
                                               instantiations.begin(),
                                               instantiations.end());
                blob.sym_instantiation_ends.push_back(
                    static_cast<std::uint32_t>(blob.sym_instantiations.size()));
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
        append_file_set(blob.sym_bitmaps, references);
        blob.sym_bitmap_ends.push_back(static_cast<std::uint32_t>(blob.sym_bitmaps.size()));
        if(!symbol->instantiation_files.isEmpty()) {
            Bitmap instantiations;
            for(auto id: symbol->instantiation_files) {
                instantiations.add(path_id(Fid{id}));
            }
            blob.sym_instantiation_docs.push_back(i);
            append_file_set(blob.sym_instantiations, instantiations);
            blob.sym_instantiation_ends.push_back(
                static_cast<std::uint32_t>(blob.sym_instantiations.size()));
        }
    }
    write_ids(blob.sym_parents, parents, entries.size() + 1);

    auto write_file_units = [&](const auto& table,
                                auto units_of,
                                std::vector<std::uint32_t>& files,
                                std::vector<std::uint32_t>& ends,
                                std::vector<std::uint8_t>& sets) {
        std::vector<std::pair<std::uint32_t, Bitmap>> reverse;
        reverse.reserve(table.size());
        for(auto& [file, units]: table) {
            Bitmap tus;
            for(auto tu: units_of(units)) {
                tus.add(path_id(tu));
            }
            reverse.emplace_back(path_id(file), std::move(tus));
        }
        std::ranges::sort(reverse, {}, [](const auto& entry) { return entry.first; });
        for(auto& [file, tus]: reverse) {
            files.push_back(file);
            append_file_set(sets, tus);
            ends.push_back(static_cast<std::uint32_t>(sets.size()));
        }
    };
    write_file_units(
        contributions,
        [](const auto& units) { return llvm::make_first_range(units); },
        blob.contributed_files,
        blob.contributor_ends,
        blob.contributors);
    write_file_units(
        fanout_units,
        [](const auto& units) { return llvm::make_range(units.begin(), units.end()); },
        blob.fanout_files,
        blob.fanout_unit_ends,
        blob.fanout_units);

    blob.search_generation = search_generation;
    blob.search_pending.assign(search_pending.begin(), search_pending.end());
    llvm::sort(blob.search_pending);
    for(auto file: search_pending_files) {
        blob.search_pending_files.push_back(path_id(file));
    }
    llvm::sort(blob.search_pending_files);
    // After every path id is handed out: the width follows the path table.
    write_ids(blob.sym_files, file_ids, blob.paths.size() + 1);
    write_ids(blob.sym_reference_counts, reference_counts, blob.paths.size() + 1);

    serialize_blob(blob, os);
}

void ProjectIndex::rebase(std::unique_ptr<llvm::MemoryBuffer> blob, FileTable& files) {
    [[maybe_unused]] bool bound = bind_global(std::move(blob), files);
    assert(bound && "a freshly serialized global blob must bind");
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
    for(auto& fv: manifest.imports) {
        if(!import(fv.raw)) {
            return false;
        }
    }
    for(auto& use: manifest.instantiations) {
        if(!import(use.file.raw)) {
            return false;
        }
    }
    for(auto& fanout: manifest.local_fanout) {
        for(auto& fv: fanout.files) {
            if(!import(fv.raw)) {
                return false;
            }
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
    for(auto& fv: exported.imports) {
        fv.raw = persisted_id(fv);
    }
    for(auto& use: exported.instantiations) {
        use.file.raw = persisted_id(use.file);
    }
    for(auto& fanout: exported.local_fanout) {
        for(auto& fv: fanout.files) {
            fv.raw = persisted_id(fv);
        }
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
    for(auto fv: manifest.imports) {
        importers[files.version(fv).fid].insert(tu_path_id);
    }
    for(auto& fanout: manifest.local_fanout) {
        for(auto fv: fanout.files) {
            fanout_units[files.version(fv).fid].insert(tu_path_id);
        }
    }
    for(auto& use: manifest.instantiations) {
        auto path_id = files.version(use.file).fid;
        instantiation_uses[path_id][tu_path_id] = use.use;
        affected.push_back(path_id);
    }
    units[unit_of(files, tu_path_id)] = tu_path_id;
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
        erase_unit(contributions, path_id, tu_path_id);
        affected.push_back(path_id);
    }
    for(auto fv: it->second.absent) {
        auto probe_it = probed.find(files.version(fv).fid);
        probe_it->second.erase(tu_path_id);
        if(probe_it->second.empty()) {
            probed.erase(probe_it);
        }
    }
    for(auto fv: it->second.imports) {
        erase_unit(importers, files.version(fv).fid, tu_path_id);
    }
    for(auto& fanout: it->second.local_fanout) {
        for(auto fv: fanout.files) {
            erase_unit(fanout_units, files.version(fv).fid, tu_path_id);
        }
    }
    for(auto& use: it->second.instantiations) {
        auto path_id = files.version(use.file).fid;
        erase_unit(instantiation_uses, path_id, tu_path_id);
        affected.push_back(path_id);
    }
    units.erase(unit_of(files, tu_path_id));
    manifests.erase(it);

    llvm::sort(affected);
    affected.erase(llvm::unique(affected), affected.end());
    return affected;
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_variants(Fid path_id) const {
    return distinct_hashes(contributions, path_id);
}

llvm::SmallVector<std::uint64_t> ProjectIndex::live_uses(Fid path_id) const {
    return distinct_hashes(instantiation_uses, path_id);
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
    if(base) {
        base->each_unit(base->contributors, file, visit);
    }
}

void ProjectIndex::each_fanout_unit(Fid file, llvm::function_ref<void(Fid)> visit) const {
    if(!db) {
        if(auto it = fanout_units.find(file); it != fanout_units.end()) {
            for(auto tu: it->second) {
                visit(tu);
            }
        }
        return;
    }
    if(base) {
        base->each_unit(base->fanout_units, file, visit);
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
        each_fanout_unit(pending.pop_back_val(), [&](Fid tu) {
            auto* manifest = asked.insert(tu).second ? tu_manifest(tu) : nullptr;
            if(!manifest) {
                return;
            }
            auto it =
                std::ranges::lower_bound(manifest->local_fanout, hash, {}, &LocalFanout::symbol);
            if(it == manifest->local_fanout.end() || it->symbol != hash) {
                return;
            }
            for(auto version: it->files) {
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
        // A reader adopts a manifest only under the generation the bound global
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

const InstantiationShard* ProjectIndex::instantiation_shard(Fid file) const {
    auto it = instantiations.find(file);
    if(it != instantiations.end()) {
        return it->second.loaded() ? &it->second : nullptr;
    }
    if(!db) {
        return nullptr;
    }
    auto& slot = instantiations[file];
    if(auto blob = db->read(IndexBlobKind::Instantiations, key_of(*files, file))) {
        slot = InstantiationShard::from_buffer(std::move(blob.buffer));
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
    search_pending_files.clear();
    for(auto file: base->search_pending_files) {
        search_pending_files.insert(base->remap[file]);
    }
    if(auto search = db.read(IndexBlobKind::Search, "search")) {
        bind_search(std::move(search.buffer));
    }
    this->db = &db;
    this->files = &files;
    return true;
}

void measure_global(llvm::StringRef data, FieldBytes& bytes) {
    if(GlobalBlob blob; deserialize_blob(data, blob)) {
        measure_fields(blob, bytes);
    }
}

}  // namespace clice::index
