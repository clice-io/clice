#include "index/preamble_state.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "index/serialization.h"

#include "kota/ipc/lsp/text.h"

namespace clice::index {

namespace {

/// One file covered by the preamble compilation: its rows plus content and
/// line starts for position mapping.
struct PreambleFileEntryRepr {
    std::uint32_t path_id = 0;
    FileIndex index;
    std::string content;
    std::vector<std::uint32_t> line_starts;
};

struct PreambleSymbolRepr {
    std::string name;
    SymbolKind kind;
};

/// The persisted shape of a `.pch.idx` blob. Queries run on a zero-copy
/// view of this layout; nothing is deserialized up front.
struct PreambleStateRepr {
    std::uint32_t format_version = 0;
    std::vector<std::string> paths;
    std::vector<PreambleFileEntryRepr> files;
    PreambleFileEntryRepr preamble;
    llvm::DenseMap<SymbolHash, PreambleSymbolRepr> symbols;
    std::vector<feature::DocumentLink> links;
    std::vector<std::uint32_t> inactive_regions;
    std::vector<std::uint8_t> open_conditionals;
};

using StateView = kota::codec::fbs::table_view<PreambleStateRepr>;
using FileEntryView = kota::codec::fbs::table_view<PreambleFileEntryRepr>;

/// The blob was fully verified at load(); per-query views skip that cost.
StateView root_of(const llvm::MemoryBuffer& buffer) {
    return StateView::from_verified_bytes(blob_bytes(buffer.getBuffer()));
}

PreambleState::File file_of(StateView root, FileEntryView entry) {
    auto paths = root[&PreambleStateRepr::paths];
    auto path_id = entry[&PreambleFileEntryRepr::path_id];
    auto line_starts = to_array_ref(entry[&PreambleFileEntryRepr::line_starts]);
    return PreambleState::File{
        .path = to_ref(paths[path_id]),
        .content = to_ref(entry[&PreambleFileEntryRepr::content]),
        .line_starts = std::span(line_starts.data(), line_starts.size()),
    };
}

}  // namespace

void PreambleState::serialize(CompilationUnitRef unit,
                              const TUIndex& index,
                              llvm::ArrayRef<feature::DocumentLink> links,
                              llvm::ArrayRef<std::uint32_t> inactive_regions,
                              llvm::ArrayRef<std::uint8_t> open_conditionals,
                              llvm::raw_ostream& os) {
    PreambleStateRepr repr;
    repr.format_version = preamble_format_version;
    repr.paths = index.graph.paths;

    repr.files.reserve(index.file_indices.size());
    for(auto& [fid, file_index]: index.file_indices) {
        // A file with no include edge is a synthetic buffer (predefines,
        // <command line>): it has no real path to attribute rows to, and
        // path_id() would misfile them under the source file. Real files
        // forced in via -include are not affected — clang records their
        // include edge in the predefines buffer, which is a valid
        // location (covered by ForcedIncludeServed).
        if(index.graph.include_location_id(fid) == static_cast<std::uint32_t>(-1)) {
            continue;
        }
        auto content = unit.file_content(fid);
        auto& entry = repr.files.emplace_back();
        entry.path_id = index.graph.path_id(fid);
        entry.index = file_index;
        entry.content = content;
        entry.line_starts =
            kota::ipc::lsp::build_line_starts(std::string_view(content.data(), content.size()));
    }

    // The source file is the last path in graph.paths (convention from
    // IncludeGraph). The preamble compile remaps the buffer truncated at
    // the bound, so interested_content() is exactly the preamble text the
    // PCH was built from — stored so consumers can compare it against the
    // live buffer's prefix before serving these rows.
    auto preamble_text = unit.interested_content();
    repr.preamble.path_id = static_cast<std::uint32_t>(index.graph.paths.size() - 1);
    repr.preamble.index = index.main_file_index;
    repr.preamble.content = preamble_text;
    repr.preamble.line_starts = kota::ipc::lsp::build_line_starts(
        std::string_view(preamble_text.data(), preamble_text.size()));

    for(auto& [symbol_id, symbol]: index.symbols) {
        repr.symbols[symbol_id] = PreambleSymbolRepr{.name = symbol.name, .kind = symbol.kind};
    }

    repr.links.assign(links.begin(), links.end());
    repr.inactive_regions.assign(inactive_regions.begin(), inactive_regions.end());
    repr.open_conditionals.assign(open_conditionals.begin(), open_conditionals.end());

    serialize_blob(repr, os);
}

PreambleState::PreambleState(std::unique_ptr<llvm::MemoryBuffer> buffer) :
    buffer(std::move(buffer)) {}

std::shared_ptr<PreambleState> PreambleState::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return nullptr;
    }

    // A stale or truncated blob must never crash the server. from_bytes
    // deep-verifies every offset, string, vector and table the views can
    // reach — queries then run unchecked (root_of). Blobs of a different
    // format version load as "missing" (version-less blobs read back 0)
    // and the PCH pair is rebuilt.
    auto root = StateView::from_bytes(blob_bytes((*buffer)->getBuffer()));
    if(!root.valid() || root[&PreambleStateRepr::format_version] != preamble_format_version) {
        return nullptr;
    }

    return std::shared_ptr<PreambleState>(new PreambleState(std::move(*buffer)));
}

void PreambleState::lookup(SymbolHash symbol,
                           RelationKind kind,
                           llvm::function_ref<bool(const File&, const Relation&)> callback) const {
    auto root = root_of(*buffer);
    auto paths = root[&PreambleStateRepr::paths];
    auto files = root[&PreambleStateRepr::files];

    for(std::size_t i = 0; i < files.size(); ++i) {
        auto entry = files[i];
        auto relations = entry[&PreambleFileEntryRepr::index][&FileIndex::relations];
        auto found = relations.find(symbol);
        if(!found) {
            continue;
        }

        // The verifier checks structure, not cross-references: a corrupt
        // path_id must not attribute rows to an arbitrary path.
        if(entry[&PreambleFileEntryRepr::path_id] >= paths.size()) {
            continue;
        }
        auto file = file_of(root, entry);

        auto rels = found->get<1>();
        for(std::size_t j = 0; j < rels.size(); ++j) {
            Relation relation = rels[j];
            if(RelationKind(relation.kind) & kind) {
                if(!callback(file, relation)) {
                    return;
                }
            }
        }
    }
}

llvm::StringRef PreambleState::source_path() const {
    auto root = root_of(*buffer);
    auto paths = root[&PreambleStateRepr::paths];
    if(paths.empty()) {
        return {};
    }
    // The source file is the last path, by IncludeGraph convention.
    return to_ref(paths[paths.size() - 1]);
}

llvm::StringRef PreambleState::preamble_content() const {
    auto root = root_of(*buffer);
    return to_ref(root[&PreambleStateRepr::preamble][&PreambleFileEntryRepr::content]);
}

void PreambleState::lookup_preamble(std::uint32_t offset,
                                    llvm::function_ref<bool(const Occurrence&)> callback) const {
    auto root = root_of(*buffer);
    auto occurrences =
        root[&PreambleStateRepr::preamble][&PreambleFileEntryRepr::index][&FileIndex::occurrences];

    // First occurrence whose range ends at or past the offset; entries are
    // sorted by (range.begin, range.end, target).
    std::size_t lo = 0;
    std::size_t hi = occurrences.size();
    while(lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if(occurrences[mid].range.end < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    for(; lo < occurrences.size(); ++lo) {
        Occurrence occurrence = occurrences[lo];
        if(!occurrence.range.contains(offset)) {
            break;
        }
        if(!callback(occurrence)) {
            break;
        }
    }
}

void PreambleState::lookup_preamble(SymbolHash symbol,
                                    RelationKind kind,
                                    llvm::function_ref<bool(const Relation&)> callback) const {
    auto root = root_of(*buffer);
    auto relations =
        root[&PreambleStateRepr::preamble][&PreambleFileEntryRepr::index][&FileIndex::relations];
    auto found = relations.find(symbol);
    if(!found) {
        return;
    }

    auto rels = found->get<1>();
    for(std::size_t i = 0; i < rels.size(); ++i) {
        Relation relation = rels[i];
        if(RelationKind(relation.kind) & kind) {
            if(!callback(relation)) {
                return;
            }
        }
    }
}

bool PreambleState::find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const {
    auto root = root_of(*buffer);
    auto found = root[&PreambleStateRepr::symbols].find(hash);
    if(!found) {
        return false;
    }

    auto symbol = found->get<1>();
    name = std::string(symbol[&PreambleSymbolRepr::name]);
    kind = SymbolKind(symbol[&PreambleSymbolRepr::kind]);
    return true;
}

std::vector<feature::DocumentLink> PreambleState::links() const {
    auto root = root_of(*buffer);
    auto entries = root[&PreambleStateRepr::links];

    std::vector<feature::DocumentLink> links;
    links.reserve(entries.size());
    for(std::size_t i = 0; i < entries.size(); ++i) {
        auto entry = entries[i];
        links.push_back(feature::DocumentLink{
            .range = entry[&feature::DocumentLink::range],
            .target = std::string(entry[&feature::DocumentLink::target]),
        });
    }
    return links;
}

llvm::ArrayRef<std::uint32_t> PreambleState::inactive_regions() const {
    auto root = root_of(*buffer);
    return to_array_ref(root[&PreambleStateRepr::inactive_regions]);
}

llvm::ArrayRef<std::uint8_t> PreambleState::open_conditionals() const {
    auto root = root_of(*buffer);
    return to_array_ref(root[&PreambleStateRepr::open_conditionals]);
}

}  // namespace clice::index
