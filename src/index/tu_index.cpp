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
import :support.logging;

namespace clice::index {

namespace {

using TUIndexView = kota::codec::fbs::table_view<TUIndexBlob>;

/// The buffer was fully verified at TUIndex::from_bytes; per-accessor
/// views skip that cost.
TUIndexView blob_root(llvm::StringRef data) {
    return TUIndexView::from_verified_bytes(blob_bytes(data));
}

/// The symbol table columns of a TU index blob, borrowing its bytes.
struct SymbolColumns {
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::StringRef names;
    llvm::ArrayRef<std::uint32_t> name_ends;
    llvm::StringRef args;
    llvm::ArrayRef<std::uint32_t> args_ends;
    llvm::StringRef signatures;
    llvm::ArrayRef<std::uint32_t> signature_ends;
    llvm::ArrayRef<std::uint64_t> parents;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint8_t> scopes;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint8_t> token_kinds;
    llvm::ArrayRef<std::uint32_t> token_modifiers;
    llvm::ArrayRef<std::uint32_t> files;
    llvm::ArrayRef<std::uint32_t> reference_ends;
    llvm::ArrayRef<std::uint32_t> references;
    llvm::ArrayRef<std::uint32_t> instantiation_ends;
    llvm::ArrayRef<std::uint32_t> instantiations;

    static SymbolColumns of(TUIndexView root) {
        return {
            .hashes = to_array_ref(root[&TUIndexBlob::sym_hashes]),
            .names = to_ref(root[&TUIndexBlob::sym_names]),
            .name_ends = to_array_ref(root[&TUIndexBlob::sym_name_ends]),
            .args = to_ref(root[&TUIndexBlob::sym_args]),
            .args_ends = to_array_ref(root[&TUIndexBlob::sym_args_ends]),
            .signatures = to_ref(root[&TUIndexBlob::sym_signatures]),
            .signature_ends = to_array_ref(root[&TUIndexBlob::sym_signature_ends]),
            .parents = to_array_ref(root[&TUIndexBlob::sym_parents]),
            .kinds = to_array_ref(root[&TUIndexBlob::sym_kinds]),
            .scopes = to_array_ref(root[&TUIndexBlob::sym_scopes]),
            .flags = to_array_ref(root[&TUIndexBlob::sym_flags]),
            .token_kinds = to_array_ref(root[&TUIndexBlob::sym_token_kinds]),
            .token_modifiers = to_array_ref(root[&TUIndexBlob::sym_token_modifiers]),
            .files = to_array_ref(root[&TUIndexBlob::sym_files]),
            .reference_ends = to_array_ref(root[&TUIndexBlob::sym_reference_ends]),
            .references = to_array_ref(root[&TUIndexBlob::sym_references]),
            .instantiation_ends = to_array_ref(root[&TUIndexBlob::sym_instantiation_ends]),
            .instantiations = to_array_ref(root[&TUIndexBlob::sym_instantiations]),
        };
    }

    std::optional<std::uint32_t> find(SymbolHash hash) const {
        auto it = std::ranges::lower_bound(hashes, hash);
        if(it == hashes.end() || *it != hash) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - hashes.begin());
    }

    SymbolIdentity identity(std::uint32_t entry) const {
        return {
            .name = back_to_back(names, name_ends, entry),
            .args = back_to_back(args, args_ends, entry),
            .signature = back_to_back(signatures, signature_ends, entry),
            .parent = parents[entry],
            .kind = SymbolKind(kinds[entry]),
            .scope = static_cast<SymbolScope>(scopes[entry]),
            .flags = static_cast<SymbolFlags>(flags[entry]),
            .file = files[entry],
            .token_class = {.kind = static_cast<SemanticTokenKind>(token_kinds[entry]),
                            .modifiers = token_modifiers[entry]},
        };
    }

    llvm::ArrayRef<std::uint32_t> references_of(std::uint32_t entry) const {
        return back_to_back(references, reference_ends, entry);
    }

    llvm::ArrayRef<std::uint32_t> instantiations_of(std::uint32_t entry) const {
        return back_to_back(instantiations, instantiation_ends, entry);
    }
};

}  // namespace

TUIndex TUIndex::from_bytes(llvm::StringRef data) {
    auto root = TUIndexView::from_bytes(blob_bytes(data));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting TU index: structural verification failed");
        return {};
    }
    if(root[&TUIndexBlob::format_version] != index_format_version) {
        LOG_DEBUG("Rejecting TU index: format version {}, this build reads {}",
                  root[&TUIndexBlob::format_version],
                  index_format_version);
        return {};
    }

    // Structural verification does not constrain field values; every path
    // id the merge dereferences against the path table is bounded here so
    // the accessors stay check-free. The builder ends every path table
    // with the focus file, so consumers address path_count() - 1
    // unchecked — an empty table marks a corrupt TU index.
    auto count = root[&TUIndexBlob::paths].size();
    if(count == 0) {
        LOG_DEBUG("Rejecting TU index: empty path table");
        return {};
    }
    // A parent names another node or the root; consumers index their
    // per-node tables with it unchecked.
    auto nodes = root[&TUIndexBlob::nodes];
    for(std::size_t i = 0; i < nodes.size(); i += 1) {
        IncludeNode node = nodes.at(i);
        if(node.file >= count || (node.parent != no_node && node.parent >= nodes.size())) {
            LOG_DEBUG("Rejecting TU index: include node past the path or node table");
            return {};
        }
    }
    auto symbols = SymbolColumns::of(root);
    auto symbol_count = symbols.hashes.size();
    if(symbols.name_ends.size() != symbol_count || symbols.args_ends.size() != symbol_count ||
       symbols.signature_ends.size() != symbol_count || symbols.parents.size() != symbol_count ||
       symbols.kinds.size() != symbol_count || symbols.scopes.size() != symbol_count ||
       symbols.flags.size() != symbol_count || symbols.token_kinds.size() != symbol_count ||
       symbols.token_modifiers.size() != symbol_count || !all_token_kinds(symbols.token_kinds) ||
       symbols.files.size() != symbol_count || symbols.reference_ends.size() != symbol_count ||
       symbols.instantiation_ends.size() != symbol_count ||
       !monotone_ends(symbols.name_ends, symbols.names.size()) ||
       !monotone_ends(symbols.args_ends, symbols.args.size()) ||
       !monotone_ends(symbols.signature_ends, symbols.signatures.size()) ||
       !monotone_ends(symbols.reference_ends, symbols.references.size()) ||
       !monotone_ends(symbols.instantiation_ends, symbols.instantiations.size())) {
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
    for(std::size_t i = 0; i < symbol_count; i += 1) {
        if((i != 0 && symbols.hashes[i] <= symbols.hashes[i - 1]) ||
           reserved_key(symbols.hashes[i]) || reserved_key(symbols.parents[i])) {
            LOG_DEBUG("Rejecting TU index: unsorted or reserved symbol hash, or reserved parent");
            return {};
        }
        if(symbols.files[i] != no_file && symbols.files[i] >= count) {
            LOG_DEBUG("Rejecting TU index: symbol file past the path table");
            return {};
        }
    }
    auto past_paths = [&](std::uint32_t file) {
        return file >= count;
    };
    if(llvm::any_of(symbols.references, past_paths) ||
       llvm::any_of(symbols.instantiations, past_paths)) {
        LOG_DEBUG("Rejecting TU index: reference file past the path table");
        return {};
    }
    // section_of binary-searches the section table by path id and shard_of
    // trusts the result, so the ids must ascend strictly — a repeated or
    // out-of-order id would attribute one file's index to another.
    auto sections = root[&TUIndexBlob::sections];
    std::uint32_t previous_path_id = 0;
    for(std::size_t i = 0; i < sections.size(); i += 1) {
        auto path_id = sections.at(i)[&FileSection::path_id];
        if(path_id >= count || (i != 0 && path_id <= previous_path_id)) {
            LOG_DEBUG("Rejecting TU index: section path ids are not strictly ascending");
            return {};
        }
        previous_path_id = path_id;
    }
    // The merge maps skipped keys through their file's stored keys by
    // binary search.
    auto instantiations = root[&TUIndexBlob::instantiations];
    for(std::size_t i = 0; i < instantiations.size(); i += 1) {
        auto section = instantiations.at(i);
        auto path_id = section[&InstantiationSection::path_id];
        if(path_id >= count || (i != 0 && path_id <= previous_path_id) ||
           !std::ranges::is_sorted(to_array_ref(section[&InstantiationSection::skipped]),
                                   std::less_equal{})) {
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
    auto result = from_bytes(buffer->getBuffer());
    if(result.loaded()) {
        result.owned = std::move(buffer);
    }
    return result;
}

std::int64_t TUIndex::built_at() const {
    return loaded() ? blob_root(data)[&TUIndexBlob::built_at] : 0;
}

std::uint32_t TUIndex::path_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::paths].size()) : 0;
}

llvm::StringRef TUIndex::path(std::uint32_t id) const {
    return to_ref(blob_root(data)[&TUIndexBlob::paths].at(id));
}

std::uint32_t TUIndex::absent_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::absent].size()) : 0;
}

llvm::StringRef TUIndex::absent(std::uint32_t i) const {
    return to_ref(blob_root(data)[&TUIndexBlob::absent].at(i));
}

std::uint64_t TUIndex::path_hash(std::uint32_t id) const {
    // The hash column may be shorter than the path table on a foreign
    // blob; an absent hash reads as 0, "unavailable".
    auto hashes = blob_root(data)[&TUIndexBlob::path_hashes];
    return id < hashes.size() ? hashes.at(id) : 0;
}

std::uint32_t TUIndex::node_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::nodes].size()) : 0;
}

IncludeNode TUIndex::node(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::nodes].at(i);
}

std::uint32_t TUIndex::section_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::sections].size())
                    : 0;
}

std::uint32_t TUIndex::section_path(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::sections].at(i)[&FileSection::path_id];
}

std::uint64_t TUIndex::section_hash(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::sections].at(i)[&FileSection::hash];
}

llvm::StringRef TUIndex::section_blob(std::uint32_t i) const {
    auto blob = to_array_ref(blob_root(data)[&TUIndexBlob::sections].at(i)[&FileSection::blob]);
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

bool TUIndex::shards_verify() const {
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
    return loaded()
               ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::instantiations].size())
               : 0;
}

std::uint32_t TUIndex::instantiation_path(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::instantiations].at(i)[&InstantiationSection::path_id];
}

llvm::StringRef TUIndex::instantiation_blob(std::uint32_t i) const {
    auto blob = to_array_ref(
        blob_root(data)[&TUIndexBlob::instantiations].at(i)[&InstantiationSection::blob]);
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

llvm::ArrayRef<std::uint64_t> TUIndex::skipped_instantiations(std::uint32_t i) const {
    return to_array_ref(
        blob_root(data)[&TUIndexBlob::instantiations].at(i)[&InstantiationSection::skipped]);
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

void TUIndex::iterate_instantiation_files(
    llvm::function_ref<bool(SymbolHash, llvm::ArrayRef<std::uint32_t>)> callback) const {
    if(!loaded()) {
        return;
    }
    auto symbols = SymbolColumns::of(blob_root(data));
    for(std::uint32_t entry = 0; entry < symbols.hashes.size(); entry += 1) {
        auto files = symbols.instantiations_of(entry);
        if(!files.empty() && !callback(symbols.hashes[entry], files)) {
            return;
        }
    }
}

void TUIndex::iterate_symbols(
    llvm::function_ref<bool(SymbolHash, const SymbolIdentity&, llvm::ArrayRef<std::uint32_t>)>
        callback) const {
    if(!loaded()) {
        return;
    }
    auto symbols = SymbolColumns::of(blob_root(data));
    for(std::uint32_t entry = 0; entry < symbols.hashes.size(); entry += 1) {
        if(!callback(symbols.hashes[entry],
                     symbols.identity(entry),
                     symbols.references_of(entry))) {
            return;
        }
    }
}

std::optional<SymbolIdentity> TUIndex::find_symbol(SymbolHash hash) const {
    if(!loaded()) {
        return std::nullopt;
    }
    auto symbols = SymbolColumns::of(blob_root(data));
    if(auto entry = symbols.find(hash)) {
        return symbols.identity(*entry);
    }
    for(std::uint32_t section = 0; section < section_count(); section += 1) {
        if(auto identity = section_shard(section).find_symbol(hash)) {
            return identity;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<PathFanout>> TUIndex::local_fanout(const llvm::BitVector& held) const {
    std::vector<PathFanout> result;
    bool valid = true;
    iterate_symbols(
        [&](SymbolHash hash, const SymbolIdentity& identity, llvm::ArrayRef<std::uint32_t> files) {
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

bool TUIndex::matches_prefix(llvm::StringRef text) const {
    if(!loaded()) {
        return false;
    }
    auto root = blob_root(data);
    auto size = root[&TUIndexBlob::preamble_size];
    return text.size() >= size &&
           llvm::xxh3_64bits(text.take_front(size)) == root[&TUIndexBlob::preamble_hash];
}

std::vector<DocumentLink> TUIndex::links() const {
    if(!loaded()) {
        return {};
    }
    auto entries = blob_root(data)[&TUIndexBlob::links];

    std::vector<DocumentLink> links;
    links.reserve(entries.size());
    for(std::size_t i = 0; i < entries.size(); i += 1) {
        auto entry = entries[i];
        links.push_back(DocumentLink{
            .range = entry[&DocumentLink::range],
            .target = std::string(entry[&DocumentLink::target]),
        });
    }
    return links;
}

llvm::ArrayRef<std::uint32_t> TUIndex::inactive_regions() const {
    if(!loaded()) {
        return {};
    }
    return to_array_ref(blob_root(data)[&TUIndexBlob::inactive_regions]);
}

llvm::ArrayRef<std::uint8_t> TUIndex::open_conditionals() const {
    if(!loaded()) {
        return {};
    }
    return to_array_ref(blob_root(data)[&TUIndexBlob::open_conditionals]);
}

llvm::StringRef TUIndex::preamble_diagnostics() const {
    if(!loaded()) {
        return {};
    }
    return to_ref(blob_root(data)[&TUIndexBlob::diagnostics]);
}

}  // namespace clice::index
