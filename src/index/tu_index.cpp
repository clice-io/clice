module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.format;
import :index.instantiation_shard;
import :index.shard;
import :index.tu_index;
import :semantic.token_class;
import :support.archive;
import :support.logging;

namespace clice::index {

namespace {

using Root = archive::Archived<TUIndexBlob>;

const Root& view_of(llvm::StringRef data) {
    return archive::view<TUIndexBlob>(data);
}

SymbolView symbol_at(const Root& root, std::uint32_t entry) {
    return {
        .name = slice(root.symbol_names, root.symbol_name_ends, entry),
        .args = slice(root.symbol_args, root.symbol_args_ends, entry),
        .signature = slice(root.symbol_signatures, root.symbol_signature_ends, entry),
        .parent = root.symbol_parents[entry],
        .kind = SymbolKind(root.symbol_kinds[entry]),
        .scope = static_cast<SymbolScope>(root.symbol_scopes[entry]),
        .flags = static_cast<SymbolFlags>(root.symbol_flags[entry]),
        .file = root.symbol_files[entry],
        .token_class = {.kind = static_cast<SemanticTokenKind>(root.symbol_token_kinds[entry]),
                        .modifiers = root.symbol_token_modifiers[entry]},
    };
}

}  // namespace

TUIndex TUIndex::from_bytes(llvm::StringRef data) {
    if(!readable_in_place(data)) {
        return from_buffer(llvm::MemoryBuffer::getMemBuffer(data, "", false));
    }
    auto verified = verify_root<TUIndexBlob>(data, "TU index");
    if(!verified) {
        return {};
    }
    auto& root = *verified;

    // Structural verification does not constrain field values; every path
    // id the merge dereferences against the path table is bounded here so
    // the accessors stay check-free. The builder ends every path table
    // with the focus file, so consumers address path_count() - 1
    // unchecked — an empty table marks a corrupt TU index.
    auto count = root.paths.size();
    if(count == 0) {
        LOG_DEBUG("Rejecting TU index: empty path table");
        return {};
    }
    // A parent names another node or the root; consumers index their
    // per-node tables with it unchecked.
    auto& nodes = root.nodes;
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        IncludeNode node = nodes[i];
        if(node.file >= count || (node.parent != no_node && node.parent >= nodes.size())) {
            LOG_DEBUG("Rejecting TU index: include node past the path or node table");
            return {};
        }
    }
    auto& hashes = root.symbol_hashes;
    auto& parents = root.symbol_parents;
    auto& files = root.symbol_files;
    auto& references = root.symbol_references;
    auto& instantiations = root.symbol_instantiations;
    auto& token_kinds = root.symbol_token_kinds;
    auto symbol_count = hashes.size();
    if(root.symbol_name_ends.size() != symbol_count ||
       root.symbol_args_ends.size() != symbol_count ||
       root.symbol_signature_ends.size() != symbol_count || parents.size() != symbol_count ||
       root.symbol_kinds.size() != symbol_count || root.symbol_scopes.size() != symbol_count ||
       root.symbol_flags.size() != symbol_count || token_kinds.size() != symbol_count ||
       root.symbol_token_modifiers.size() != symbol_count || !all_token_kinds(token_kinds) ||
       files.size() != symbol_count || root.symbol_reference_ends.size() != symbol_count ||
       root.symbol_instantiation_ends.size() != symbol_count ||
       !monotone_ends(root.symbol_name_ends, root.symbol_names.size()) ||
       !monotone_ends(root.symbol_args_ends, root.symbol_args.size()) ||
       !monotone_ends(root.symbol_signature_ends, root.symbol_signatures.size()) ||
       !monotone_ends(root.symbol_reference_ends, references.size()) ||
       !monotone_ends(root.symbol_instantiation_ends, instantiations.size())) {
        LOG_DEBUG("Rejecting TU index: malformed symbol columns");
        return {};
    }
    // Lookups binary-search the hashes. Hashes and parents become DenseMap
    // and DenseSet keys in every consumer (the project table, a query's
    // parent walk), and the two sentinel values corrupt or assert in those
    // containers; the builder never emits them, so a TU index carrying
    // one is corrupt. Merged reference files would persist behind versions
    // that match the disk, so a path id past the table must reject the
    // whole TU index rather than be dropped.
    for(std::uint32_t i = 0; i < symbol_count; i += 1) {
        if((i != 0 && hashes[i] <= hashes[i - 1]) || reserved_key(hashes[i]) ||
           reserved_key(parents[i])) {
            LOG_DEBUG("Rejecting TU index: unsorted or reserved symbol hash, or reserved parent");
            return {};
        }
        if(files[i] != no_file && files[i] >= count) {
            LOG_DEBUG("Rejecting TU index: symbol file past the path table");
            return {};
        }
    }
    auto past_paths = [&](std::uint32_t file) {
        return file >= count;
    };
    if(llvm::any_of(references, past_paths) || llvm::any_of(instantiations, past_paths)) {
        LOG_DEBUG("Rejecting TU index: reference file past the path table");
        return {};
    }
    // section_of binary-searches the section table by path id and shard_of
    // trusts the result, so the ids must ascend strictly — a repeated or
    // out-of-order id would attribute one file's index to another.
    auto& sections = root.sections;
    std::uint32_t previous_path_id = 0;
    for(std::uint32_t i = 0; i < sections.size(); i += 1) {
        auto path_id = sections[i].path_id;
        if(path_id >= count || (i != 0 && path_id <= previous_path_id)) {
            LOG_DEBUG("Rejecting TU index: section path ids are not strictly ascending");
            return {};
        }
        previous_path_id = path_id;
    }
    // The merge maps skipped keys through their file's stored keys by
    // binary search.
    auto& instantiation_sections = root.instantiations;
    for(std::uint32_t i = 0; i < instantiation_sections.size(); i += 1) {
        auto& section = instantiation_sections[i];
        auto path_id = section.path_id;
        if(path_id >= count || (i != 0 && path_id <= previous_path_id) ||
           !strictly_ascending(section.skipped)) {
            LOG_DEBUG("Rejecting TU index: instantiation sections are not strictly ascending");
            return {};
        }
        previous_path_id = path_id;
    }

    TUIndex result;
    result.data = data;
    return result;
}

TUIndex TUIndex::from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer) {
    if(!buffer) {
        return {};
    }
    buffer = in_place_buffer(std::move(buffer));
    auto result = from_bytes(buffer->getBuffer());
    if(result.loaded()) {
        result.owned = std::move(buffer);
    }
    return result;
}

std::int64_t TUIndex::built_at() const {
    return loaded() ? view_of(data).built_at : 0;
}

std::uint32_t TUIndex::path_count() const {
    return loaded() ? static_cast<std::uint32_t>(view_of(data).paths.size()) : 0;
}

llvm::StringRef TUIndex::path(std::uint32_t id) const {
    return view_of(data).paths[id];
}

std::uint32_t TUIndex::absent_count() const {
    return loaded() ? static_cast<std::uint32_t>(view_of(data).absent.size()) : 0;
}

llvm::StringRef TUIndex::absent(std::uint32_t i) const {
    return view_of(data).absent[i];
}

std::uint64_t TUIndex::content_hash(std::uint32_t id) const {
    // The hash column may be shorter than the path table on a foreign
    // blob; an absent hash reads as 0, "unavailable".
    auto& hashes = view_of(data).content_hashes;
    return id < hashes.size() ? hashes[id] : 0;
}

std::uint32_t TUIndex::node_count() const {
    return loaded() ? static_cast<std::uint32_t>(view_of(data).nodes.size()) : 0;
}

IncludeNode TUIndex::node(std::uint32_t i) const {
    return view_of(data).nodes[i];
}

std::uint32_t TUIndex::section_count() const {
    return loaded() ? static_cast<std::uint32_t>(view_of(data).sections.size()) : 0;
}

std::uint32_t TUIndex::section_path(std::uint32_t i) const {
    return view_of(data).sections[i].path_id;
}

std::uint64_t TUIndex::section_hash(std::uint32_t i) const {
    return view_of(data).sections[i].hash;
}

llvm::StringRef TUIndex::section_blob(std::uint32_t i) const {
    auto& blob = view_of(data).sections[i].blob;
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

std::optional<std::uint32_t> TUIndex::section_of(std::uint32_t path_id) const {
    auto sections = std::views::iota(std::uint32_t(0), section_count());
    auto it = std::ranges::lower_bound(sections, path_id, {}, [&](std::uint32_t i) {
        return section_path(i);
    });
    if(it == sections.end() || section_path(*it) != path_id) {
        return std::nullopt;
    }
    return *it;
}

const Shard& TUIndex::shard_of(std::uint32_t path_id) const {
    const static Shard missing;
    auto section = section_of(path_id);
    return section ? section_shard(*section) : missing;
}

const Shard& TUIndex::section_shard(std::uint32_t section) const {
    if(shards.empty()) {
        shards.resize(section_count());
    }
    auto& slot = shards[section];
    if(!slot.loaded()) {
        slot = Shard::from_bytes(section_blob(section));
    }
    return slot;
}

bool TUIndex::verify_shards() const {
    if(shards.empty()) {
        shards.resize(section_count());
    }
    for(std::uint32_t i = 0; i < section_count(); i += 1) {
        // Structural verification alone accepts flipped bits that still
        // form a valid shard (an in-bounds range, another symbol id);
        // only the byte hash catches those, so a persisted TU index must
        // fail here and rebuild instead of serving corrupted entries.
        if(llvm::xxh3_64bits(section_blob(i)) != section_hash(i)) {
            return false;
        }
        if(!shards[i].loaded()) {
            shards[i] = Shard::from_bytes(section_blob(i));
            if(!shards[i].loaded()) {
                return false;
            }
        }
    }
    return true;
}

std::uint32_t TUIndex::instantiation_count() const {
    return loaded() ? static_cast<std::uint32_t>(view_of(data).instantiations.size()) : 0;
}

std::uint32_t TUIndex::instantiation_path(std::uint32_t i) const {
    return view_of(data).instantiations[i].path_id;
}

llvm::StringRef TUIndex::instantiation_blob(std::uint32_t i) const {
    auto& blob = view_of(data).instantiations[i].blob;
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

llvm::ArrayRef<std::uint64_t> TUIndex::skipped_instantiations(std::uint32_t i) const {
    return view_of(data).instantiations[i].skipped;
}

const InstantiationShard& TUIndex::instantiation_shard(std::uint32_t i) const {
    if(instantiation_shards.empty()) {
        instantiation_shards.resize(instantiation_count());
    }
    auto& slot = instantiation_shards[i];
    if(!slot.loaded() && !instantiation_blob(i).empty()) {
        slot = InstantiationShard::from_bytes(instantiation_blob(i));
    }
    return slot;
}

std::optional<std::uint32_t> TUIndex::instantiation_section_of(std::uint32_t path_id) const {
    auto sections = std::views::iota(std::uint32_t(0), instantiation_count());
    auto it = std::ranges::lower_bound(sections, path_id, {}, [&](std::uint32_t i) {
        return instantiation_path(i);
    });
    if(it == sections.end() || instantiation_path(*it) != path_id) {
        return std::nullopt;
    }
    return *it;
}

void TUIndex::for_each_instantiation_file(
    llvm::function_ref<bool(SymbolHash, llvm::ArrayRef<std::uint32_t>)> callback) const {
    if(!loaded()) {
        return;
    }
    auto& root = view_of(data);
    auto& hashes = root.symbol_hashes;
    for(std::uint32_t entry = 0; entry < hashes.size(); entry += 1) {
        auto files = slice(root.symbol_instantiations, root.symbol_instantiation_ends, entry);
        if(!files.empty() && !callback(hashes[entry], files)) {
            return;
        }
    }
}

void TUIndex::for_each_symbol(
    llvm::function_ref<bool(SymbolHash, const SymbolView&, llvm::ArrayRef<std::uint32_t>)> callback)
    const {
    if(!loaded()) {
        return;
    }
    auto& root = view_of(data);
    auto& hashes = root.symbol_hashes;
    for(std::uint32_t entry = 0; entry < hashes.size(); entry += 1) {
        auto references = slice(root.symbol_references, root.symbol_reference_ends, entry);
        if(!callback(hashes[entry], symbol_at(root, entry), references)) {
            return;
        }
    }
}

std::optional<SymbolView> TUIndex::find_symbol(SymbolHash hash) const {
    if(!loaded()) {
        return std::nullopt;
    }
    auto& root = view_of(data);
    if(auto entry = sorted_find(root.symbol_hashes, hash)) {
        return symbol_at(root, *entry);
    }
    for(std::uint32_t section = 0; section < section_count(); section += 1) {
        if(auto identity = section_shard(section).find_symbol(hash)) {
            return identity;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<TUSharedLocal>>
    TUIndex::shared_locals(const llvm::BitVector& held) const {
    std::vector<TUSharedLocal> result;
    bool valid = true;
    for_each_symbol(
        [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t> files) {
            if(identity.scope == SymbolScope::External || files.size() < 2) {
                return true;
            }
            if(llvm::any_of(files, [&](std::uint32_t path_id) { return !held.test(path_id); })) {
                valid = false;
                return false;
            }
            result.push_back({
                .symbol = hash,
                .paths = {files.begin(), files.end()}
            });
            return true;
        });
    if(!valid) {
        return std::nullopt;
    }
    return result;
}

bool TUIndex::matches_preamble(llvm::StringRef text) const {
    if(!loaded()) {
        return false;
    }
    auto& root = view_of(data);
    auto& size = root.preamble_size;
    return text.size() >= size && llvm::xxh3_64bits(text.take_front(size)) == root.preamble_hash;
}

std::vector<DocumentLink> TUIndex::links() const {
    if(!loaded()) {
        return {};
    }
    auto& entries = view_of(data).links;

    std::vector<DocumentLink> links;
    links.reserve(entries.size());
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        auto& entry = entries[i];
        links.push_back(DocumentLink{
            .range = entry.range,
            .target = entry.target.str(),
        });
    }
    return links;
}

llvm::ArrayRef<std::uint32_t> TUIndex::inactive_regions() const {
    if(!loaded()) {
        return {};
    }
    return view_of(data).inactive_regions;
}

llvm::ArrayRef<std::uint8_t> TUIndex::open_conditionals() const {
    if(!loaded()) {
        return {};
    }
    return view_of(data).open_conditionals;
}

llvm::StringRef TUIndex::preamble_diagnostics() const {
    if(!loaded()) {
        return {};
    }
    return view_of(data).diagnostics;
}

}  // namespace clice::index
