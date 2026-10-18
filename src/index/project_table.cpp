module;

#include "modules/prelude.h"

module clice;

import :index.project_table;

namespace clice::index {

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

std::expected<void, llvm::StringRef> ProjectTable::bind(kota::codec::fbs::table_view<GlobalBlob> root) {
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
    contributed_files = to_array_ref(root[&GlobalBlob::contributed_files]);
    contributor_ends = to_array_ref(root[&GlobalBlob::contributor_ends]);
    contributors = to_array_ref(root[&GlobalBlob::contributors]);
    shared_local_symbols = to_array_ref(root[&GlobalBlob::shared_local_symbols]);
    shared_local_set_ends = to_array_ref(root[&GlobalBlob::shared_local_set_ends]);
    shared_local_file_ends = to_array_ref(root[&GlobalBlob::shared_local_file_ends]);
    shared_local_files = to_array_ref(root[&GlobalBlob::shared_local_files]);

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
    if(shared_local_set_ends.size() != shared_local_symbols.size() ||
       !covering_ends(shared_local_set_ends, shared_local_file_ends.size()) ||
       !covering_ends(shared_local_file_ends, shared_local_files.size()) ||
       !std::ranges::is_sorted(shared_local_symbols, std::less_equal{})) {
        return std::unexpected("shared local columns do not line up");
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

void measure_global(llvm::StringRef data, FieldBytes& bytes) {
    if(GlobalBlob blob; deserialize_blob(data, blob)) {
        measure_fields(blob, bytes);
    }
}

}  // namespace clice::index
