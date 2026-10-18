module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_table;
import :semantic.token_class;

namespace clice::index {

/// Most file sets hold one or two files: up to raw_file_set_limit path ids
/// are stored raw as u32s, more as a portable roaring image — which is
/// longer than any raw list, so a slice's length says which it is.
constexpr static std::uint32_t raw_file_set_limit = 2;

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
        auto image = read_bitmap(bytes.data(), bytes.size());
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

std::expected<void, llvm::StringRef>
    ProjectTable::bind(kota::codec::fbs::table_view<ProjectTableBlob> root) {
    if(root[&ProjectTableBlob::format_version] != index_format_version) {
        return std::unexpected("written by another index format version");
    }
    generation = root[&ProjectTableBlob::generation];
    next_version_id = root[&ProjectTableBlob::next_version_id];
    version_ids = to_array_ref(root[&ProjectTableBlob::version_ids]);
    version_paths = root[&ProjectTableBlob::version_paths];
    version_hashes = to_array_ref(root[&ProjectTableBlob::version_hashes]);
    paths = root[&ProjectTableBlob::paths];
    hashes = to_array_ref(root[&ProjectTableBlob::symbol_hashes]);
    names = to_ref(root[&ProjectTableBlob::symbol_names]);
    name_ends = to_array_ref(root[&ProjectTableBlob::symbol_name_ends]);
    args_entries = to_array_ref(root[&ProjectTableBlob::symbol_args_entries]);
    args = to_ref(root[&ProjectTableBlob::symbol_args]);
    args_ends = to_array_ref(root[&ProjectTableBlob::symbol_args_ends]);
    signatures = to_ref(root[&ProjectTableBlob::symbol_signatures]);
    signature_ends = to_array_ref(root[&ProjectTableBlob::symbol_signature_ends]);
    parents = ids_of(root[&ProjectTableBlob::symbol_parents], hashes.size() + 1);
    far_parents = to_array_ref(root[&ProjectTableBlob::symbol_far_parents]);
    far_parent_hashes = to_array_ref(root[&ProjectTableBlob::symbol_far_parent_hashes]);
    kinds = to_array_ref(root[&ProjectTableBlob::symbol_kinds]);
    flags = to_array_ref(root[&ProjectTableBlob::symbol_flags]);
    token_kinds = to_array_ref(root[&ProjectTableBlob::symbol_token_kinds]);
    token_modifiers = to_array_ref(root[&ProjectTableBlob::symbol_token_modifiers]);
    files = ids_of(root[&ProjectTableBlob::symbol_files], paths.size() + 1);
    reference_counts = ids_of(root[&ProjectTableBlob::symbol_reference_counts], paths.size() + 1);
    bitmap_ends = to_array_ref(root[&ProjectTableBlob::symbol_bitmap_ends]);
    bitmaps = to_array_ref(root[&ProjectTableBlob::symbol_bitmaps]);
    instantiation_entries = to_array_ref(root[&ProjectTableBlob::symbol_instantiation_entries]);
    instantiation_ends = to_array_ref(root[&ProjectTableBlob::symbol_instantiation_ends]);
    instantiation_images = to_array_ref(root[&ProjectTableBlob::symbol_instantiations]);
    manifest_versions = to_array_ref(root[&ProjectTableBlob::manifest_versions]);
    manifest_generations = to_array_ref(root[&ProjectTableBlob::manifest_generations]);
    search_generation = root[&ProjectTableBlob::search_generation];
    search_pending = to_array_ref(root[&ProjectTableBlob::search_pending]);
    search_pending_files = to_array_ref(root[&ProjectTableBlob::search_pending_files]);
    contributed_files = to_array_ref(root[&ProjectTableBlob::contributed_files]);
    contributor_ends = to_array_ref(root[&ProjectTableBlob::contributor_ends]);
    contributors = to_array_ref(root[&ProjectTableBlob::contributors]);
    shared_local_symbols = to_array_ref(root[&ProjectTableBlob::shared_local_symbols]);
    shared_local_set_ends = to_array_ref(root[&ProjectTableBlob::shared_local_set_ends]);
    shared_local_file_ends = to_array_ref(root[&ProjectTableBlob::shared_local_file_ends]);
    shared_local_files = to_array_ref(root[&ProjectTableBlob::shared_local_files]);

    auto version_count = version_ids.size();
    if(version_paths.size() != version_count || version_hashes.size() != version_count) {
        return std::unexpected("file version columns do not line up");
    }
    if(!std::ranges::is_sorted(version_ids)) {
        return std::unexpected("file version ids are not ascending");
    }
    if(contributor_ends.size() != contributed_files.size() ||
       !monotone_ends(contributor_ends, contributors.size())) {
        return std::unexpected("contributor columns do not line up");
    }
    if(!strictly_ascending(contributed_files) ||
       (!contributed_files.empty() && contributed_files.back() >= paths.size())) {
        return std::unexpected("contributed files are not ascending path ids");
    }
    if(shared_local_set_ends.size() != shared_local_symbols.size() ||
       !covering_ends(shared_local_set_ends, shared_local_file_ends.size()) ||
       !covering_ends(shared_local_file_ends, shared_local_files.size()) ||
       !strictly_ascending(shared_local_symbols)) {
        return std::unexpected("shared local columns do not line up");
    }
    if(manifest_versions.size() != manifest_generations.size()) {
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
    auto sparse = [&](llvm::ArrayRef<std::uint32_t> entries, std::size_t values) {
        return sparse_ok(entries, values, count);
    };
    if(!sparse(args_entries, args_ends.size()) ||
       !sparse(instantiation_entries, instantiation_ends.size()) ||
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
    if(!strictly_ascending(hashes) || llvm::any_of(hashes, reserved_key<SymbolHash>)) {
        return std::unexpected("symbol hashes are not ascending");
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

void measure_table(llvm::StringRef data, FieldBytes& bytes) {
    if(ProjectTableBlob blob; deserialize_blob(data, blob)) {
        measure_fields(blob, bytes);
    }
}

}  // namespace clice::index
