#include "index/preamble_index.h"

#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "index/serialization.h"

#include "llvm/Support/xxhash.h"

namespace clice::index {

namespace {

/// One file covered by the preamble compilation: its shard blob, embedded
/// verbatim from the consumed TUIndex section. Entries are only ever
/// encoded — queries run on Shard readers wrapped at load().
struct PreambleEntry {
    std::uint32_t path_id = 0;
    llvm::ArrayRef<std::uint8_t> blob;
};

/// find_symbol serves only name and kind, so the blob stores this reduced
/// entry instead of the full Symbol — reflecting that would drag every
/// symbol's scope and reference bitmap into large SDK preamble blobs for
/// nothing. The name borrows the consumed TUIndex (encode-only, like
/// PreambleEntry).
struct PreambleSymbol {
    llvm::StringRef name;
    SymbolKind kind;
};

/// The persisted shape of a `.pch.idx` blob.
struct PreambleBlob {
    std::uint32_t format_version = 0;

    /// Identity of the exact preamble text the PCH was built from:
    /// xxh3 and byte size. Consumers serve preamble-derived state only
    /// while the live buffer's prefix still matches (matches_prefix) —
    /// independent of whether the region produced any rows.
    std::uint64_t preamble_hash = 0;
    std::uint32_t preamble_size = 0;

    std::vector<std::string> paths;
    std::vector<PreambleEntry> files;
    PreambleEntry preamble;
    llvm::DenseMap<SymbolHash, PreambleSymbol> symbols;
    llvm::ArrayRef<feature::DocumentLink> links;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    llvm::ArrayRef<std::uint8_t> open_conditionals;
};

using BlobView = kota::codec::fbs::table_view<PreambleBlob>;

/// The blob was fully verified at load(); per-query views skip that cost.
BlobView root_of(const llvm::MemoryBuffer& buffer) {
    return BlobView::from_verified_bytes(blob_bytes(buffer.getBuffer()));
}

llvm::StringRef entry_bytes(kota::codec::fbs::table_view<PreambleEntry> entry) {
    auto blob = to_array_ref(entry[&PreambleEntry::blob]);
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

}  // namespace

void PreambleIndex::serialize(CompilationUnitRef unit,
                              TUIndex index,
                              llvm::ArrayRef<feature::DocumentLink> links,
                              llvm::ArrayRef<std::uint32_t> inactive_regions,
                              llvm::ArrayRef<std::uint8_t> open_conditionals,
                              llvm::raw_ostream& os) {
    PreambleBlob blob;
    blob.format_version = preamble_format_version;

    // The preamble compile remaps the buffer truncated at the bound, so
    // interested_content() is exactly the preamble text the PCH was built
    // from.
    auto preamble_text = unit.interested_content();
    blob.preamble_hash = llvm::xxh3_64bits(preamble_text);
    blob.preamble_size = static_cast<std::uint32_t>(preamble_text.size());

    // The source file is the last path in graph.paths (convention from
    // IncludeGraph); its section holds the preamble region's own rows.
    auto main_id = static_cast<std::uint32_t>(index.graph.paths.size()) - 1;
    blob.files.reserve(index.sections.size());
    for(auto& section: index.sections) {
        if(section.path_id == main_id) {
            blob.preamble = {section.path_id, section.blob};
        } else {
            blob.files.push_back({section.path_id, section.blob});
        }
    }

    blob.symbols.reserve(index.symbols.size());
    for(const auto& [hash, symbol]: index.symbols) {
        blob.symbols.try_emplace(hash, PreambleSymbol{.name = symbol.name, .kind = symbol.kind});
    }
    blob.paths = std::move(index.graph.paths);
    blob.links = links;
    blob.inactive_regions = inactive_regions;
    blob.open_conditionals = open_conditionals;

    serialize_blob(blob, os);
}

std::shared_ptr<PreambleIndex> PreambleIndex::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return nullptr;
    }

    // A stale or truncated blob must never crash the server. from_bytes
    // deep-verifies every offset, string, vector and table the views can
    // reach, and each embedded shard blob is verified once by the Shard
    // wrap below — queries then run unchecked. Anything failing loads as
    // "missing" and the PCH pair is rebuilt.
    auto root = BlobView::from_bytes(blob_bytes((*buffer)->getBuffer()));
    if(!root.valid() || root[&PreambleBlob::format_version] != preamble_format_version) {
        return nullptr;
    }

    std::shared_ptr<PreambleIndex> state(new PreambleIndex());
    state->buffer = std::move(*buffer);

    auto verified = root_of(*state->buffer);
    auto paths = verified[&PreambleBlob::paths];
    auto files = verified[&PreambleBlob::files];
    state->file_shards.reserve(files.size());
    state->file_paths.reserve(files.size());
    for(std::size_t i = 0; i < files.size(); i += 1) {
        auto entry = files[i];
        if(entry[&PreambleEntry::path_id] >= paths.size()) {
            return nullptr;
        }
        auto shard = Shard::from_bytes(entry_bytes(entry));
        if(!shard.loaded()) {
            return nullptr;
        }
        state->file_paths.push_back(to_ref(paths[entry[&PreambleEntry::path_id]]));
        state->file_shards.push_back(std::move(shard));
    }

    // The preamble region may legitimately have no rows — an absent blob
    // stays an empty shard; corrupt bytes still reject the pair.
    auto preamble = entry_bytes(verified[&PreambleBlob::preamble]);
    if(!preamble.empty()) {
        state->preamble_shard = Shard::from_bytes(preamble);
        if(!state->preamble_shard.loaded()) {
            return nullptr;
        }
    }

    return state;
}

void PreambleIndex::lookup(SymbolHash symbol,
                           RelationKind kind,
                           llvm::function_ref<bool(const File&, const Relation&)> callback) const {
    for(std::size_t i = 0; i < file_shards.size(); i += 1) {
        auto& shard = file_shards[i];
        File file{
            .path = file_paths[i],
            .content = shard.content(),
            .content_size = shard.content_size(),
            .line_starts = shard.line_starts(),
        };
        bool stopped = false;
        shard.lookup(symbol, kind, [&](const Relation& relation) {
            if(!callback(file, relation)) {
                stopped = true;
                return false;
            }
            return true;
        });
        if(stopped) {
            return;
        }
    }
}

llvm::StringRef PreambleIndex::source_path() const {
    auto root = root_of(*buffer);
    auto paths = root[&PreambleBlob::paths];
    if(paths.empty()) {
        return {};
    }
    // The source file is the last path, by IncludeGraph convention.
    return to_ref(paths[paths.size() - 1]);
}

bool PreambleIndex::matches_prefix(llvm::StringRef text) const {
    auto root = root_of(*buffer);
    auto size = root[&PreambleBlob::preamble_size];
    return text.size() >= size &&
           llvm::xxh3_64bits(text.take_front(size)) == root[&PreambleBlob::preamble_hash];
}

void PreambleIndex::lookup_preamble(std::uint32_t offset,
                                    llvm::function_ref<bool(const Occurrence&)> callback) const {
    preamble_shard.lookup(offset, callback);
}

void PreambleIndex::lookup_preamble(SymbolHash symbol,
                                    RelationKind kind,
                                    llvm::function_ref<bool(const Relation&)> callback) const {
    preamble_shard.lookup(symbol, kind, callback);
}

bool PreambleIndex::find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const {
    auto root = root_of(*buffer);
    auto found = root[&PreambleBlob::symbols].find(hash);
    if(!found) {
        return false;
    }

    auto symbol = found->get<1>();
    name = std::string(symbol[&PreambleSymbol::name]);
    kind = SymbolKind(symbol[&PreambleSymbol::kind]);
    return true;
}

std::vector<feature::DocumentLink> PreambleIndex::links() const {
    auto root = root_of(*buffer);
    auto entries = root[&PreambleBlob::links];

    std::vector<feature::DocumentLink> links;
    links.reserve(entries.size());
    for(std::size_t i = 0; i < entries.size(); i += 1) {
        auto entry = entries[i];
        links.push_back(feature::DocumentLink{
            .range = entry[&feature::DocumentLink::range],
            .target = std::string(entry[&feature::DocumentLink::target]),
        });
    }
    return links;
}

llvm::ArrayRef<std::uint32_t> PreambleIndex::inactive_regions() const {
    auto root = root_of(*buffer);
    return to_array_ref(root[&PreambleBlob::inactive_regions]);
}

llvm::ArrayRef<std::uint8_t> PreambleIndex::open_conditionals() const {
    auto root = root_of(*buffer);
    return to_array_ref(root[&PreambleBlob::open_conditionals]);
}

}  // namespace clice::index
