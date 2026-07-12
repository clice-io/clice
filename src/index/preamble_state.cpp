#include "index/preamble_state.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "index/serialization.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/ADT/SmallVector.h"

namespace clice::index {

namespace {

/// Serialize one FileIndex into a PreambleFileEntry, with relations sorted
/// by symbol hash so the read path can binary-search them.
fbs::Offset<binary::PreambleFileEntry> serialize_entry(fbs::FlatBufferBuilder& builder,
                                                       std::uint32_t path_id,
                                                       const FileIndex& index,
                                                       llvm::StringRef content,
                                                       llvm::ArrayRef<std::uint32_t> line_starts) {
    auto occs = CreateStructVector<binary::Occurrence>(builder, index.occurrences);

    llvm::SmallVector<std::pair<SymbolHash, const std::vector<Relation>*>, 0> sorted;
    sorted.reserve(index.relations.size());
    for(auto& [symbol_id, relations]: index.relations) {
        sorted.emplace_back(symbol_id, &relations);
    }
    std::ranges::sort(sorted, {}, [](const auto& entry) { return entry.first; });

    auto rels = transform(sorted, [&](const auto& entry) {
        return binary::CreateTUFileRelationsEntry(
            builder,
            entry.first,
            CreateStructVector<binary::Relation>(builder, *entry.second));
    });

    return binary::CreatePreambleFileEntry(
        builder,
        path_id,
        occs,
        CreateVector(builder, rels),
        content.empty() ? 0 : CreateString(builder, content),
        line_starts.empty() ? 0 : CreateVector(builder, line_starts));
}

}  // namespace

void PreambleState::serialize(CompilationUnitRef unit,
                              const TUIndex& index,
                              llvm::ArrayRef<feature::DocumentLink> links,
                              llvm::ArrayRef<std::uint32_t> inactive_regions,
                              llvm::ArrayRef<std::uint8_t> open_conditionals,
                              llvm::raw_ostream& os) {
    fbs::FlatBufferBuilder builder(4096);

    auto paths =
        transform(index.graph.paths, [&](const std::string& p) { return builder.CreateString(p); });

    Offsets<binary::PreambleFileEntry> files;
    files.reserve(index.file_indices.size());
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
        auto line_starts =
            kota::ipc::lsp::build_line_starts(std::string_view(content.data(), content.size()));
        files.push_back(
            serialize_entry(builder, index.graph.path_id(fid), file_index, content, line_starts));
    }

    // The source file is the last path in graph.paths (convention from
    // IncludeGraph). The preamble compile remaps the buffer truncated at
    // the bound, so interested_content() is exactly the preamble text the
    // PCH was built from — stored so consumers can compare it against the
    // live buffer's prefix before serving these rows.
    auto preamble_text = unit.interested_content();
    auto preamble_starts = kota::ipc::lsp::build_line_starts(
        std::string_view(preamble_text.data(), preamble_text.size()));
    auto preamble_entry = serialize_entry(builder,
                                          static_cast<std::uint32_t>(index.graph.paths.size() - 1),
                                          index.main_file_index,
                                          preamble_text,
                                          preamble_starts);

    llvm::SmallVector<std::pair<SymbolHash, const Symbol*>, 0> sorted_symbols;
    sorted_symbols.reserve(index.symbols.size());
    for(auto& [symbol_id, symbol]: index.symbols) {
        sorted_symbols.emplace_back(symbol_id, &symbol);
    }
    std::ranges::sort(sorted_symbols, {}, [](const auto& entry) { return entry.first; });

    auto syms = transform(sorted_symbols, [&](const auto& entry) {
        return binary::CreatePreambleSymbolEntry(builder,
                                                 entry.first,
                                                 CreateString(builder, entry.second->name),
                                                 entry.second->kind.value());
    });

    auto link_entries = transform(links, [&](const feature::DocumentLink& link) {
        binary::Range range(link.range.begin, link.range.end);
        return binary::CreatePreambleDocumentLink(builder,
                                                  &range,
                                                  CreateString(builder, link.target));
    });

    auto root = binary::CreatePreambleState(builder,
                                            preamble_format_version,
                                            CreateVector(builder, paths),
                                            CreateVector(builder, files),
                                            preamble_entry,
                                            CreateVector(builder, syms),
                                            CreateVector(builder, link_entries),
                                            CreateVector(builder, inactive_regions),
                                            CreateVector(builder, open_conditionals));
    builder.Finish(root);
    os.write(safe_cast<const char>(builder.GetBufferPointer()), builder.GetSize());
}

PreambleState::PreambleState(std::unique_ptr<llvm::MemoryBuffer> buffer) :
    buffer(std::move(buffer)) {}

std::shared_ptr<PreambleState> PreambleState::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return nullptr;
    }

    // A stale or truncated blob must never crash the server. Verify it is
    // a structurally valid flatbuffer, then discard any blob whose format
    // version differs (version-less blobs read back 0). The table budget
    // is far above the default: a large preamble's symbol table alone can
    // exceed a million entries, and a verification failure here would
    // otherwise send every compile into a rebuild loop.
    auto data = reinterpret_cast<const std::uint8_t*>((*buffer)->getBufferStart());
    fbs::Verifier verifier(data,
                           (*buffer)->getBufferSize(),
                           /*max_depth=*/64,
                           /*max_tables=*/1u << 26);
    if(!verifier.VerifyBuffer<binary::PreambleState>(nullptr)) {
        return nullptr;
    }

    auto root = fbs::GetRoot<binary::PreambleState>((*buffer)->getBufferStart());
    if(root->format_version() != preamble_format_version) {
        return nullptr;
    }

    return std::shared_ptr<PreambleState>(new PreambleState(std::move(*buffer)));
}

void PreambleState::lookup(SymbolHash symbol,
                           RelationKind kind,
                           llvm::function_ref<bool(const File&, const Relation&)> callback) const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto paths = root->paths();

    for(auto entry: *root->files()) {
        auto rels = entry->relations();
        if(!rels) {
            continue;
        }

        auto it = std::ranges::lower_bound(*rels, symbol, {}, [](auto e) { return e->symbol(); });
        if(it == rels->end() || it->symbol() != symbol || !it->relations()) {
            continue;
        }

        // The verifier checks structure, not cross-references: a corrupt
        // path_id would index out of bounds on the mapped file.
        if(entry->path_id() >= paths->size()) {
            continue;
        }
        auto path = paths->Get(entry->path_id());
        File file{
            .path = llvm::StringRef(path->c_str(), path->size()),
            .content = entry->content()
                           ? llvm::StringRef(entry->content()->c_str(), entry->content()->size())
                           : llvm::StringRef(),
            .line_starts = entry->line_starts() ? std::span(entry->line_starts()->data(),
                                                            entry->line_starts()->size())
                                                : std::span<const std::uint32_t>(),
        };

        for(auto rel: *it->relations()) {
            auto r = safe_cast<Relation>(rel);
            if(r->kind & kind) {
                if(!callback(file, *r)) {
                    return;
                }
            }
        }
    }
}

llvm::StringRef PreambleState::source_path() const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto paths = root->paths();
    if(paths->size() == 0) {
        return {};
    }
    // The source file is the last path, by IncludeGraph convention.
    auto path = paths->Get(paths->size() - 1);
    return llvm::StringRef(path->c_str(), path->size());
}

llvm::StringRef PreambleState::preamble_content() const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto preamble = root->preamble();
    if(!preamble || !preamble->content()) {
        return {};
    }
    return llvm::StringRef(preamble->content()->c_str(), preamble->content()->size());
}

void PreambleState::lookup_preamble(std::uint32_t offset,
                                    llvm::function_ref<bool(const Occurrence&)> callback) const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto preamble = root->preamble();
    if(!preamble || !preamble->occurrences()) {
        return;
    }

    auto& occurrences = *preamble->occurrences();
    auto it =
        std::ranges::lower_bound(occurrences, offset, {}, [](auto o) { return o->range().end(); });

    while(it != occurrences.end()) {
        auto o = safe_cast<Occurrence>(*it);
        if(!o->range.contains(offset)) {
            break;
        }
        if(!callback(*o)) {
            break;
        }
        ++it;
    }
}

void PreambleState::lookup_preamble(SymbolHash symbol,
                                    RelationKind kind,
                                    llvm::function_ref<bool(const Relation&)> callback) const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto preamble = root->preamble();
    if(!preamble || !preamble->relations()) {
        return;
    }

    auto& rels = *preamble->relations();
    auto it = std::ranges::lower_bound(rels, symbol, {}, [](auto e) { return e->symbol(); });
    if(it == rels.end() || it->symbol() != symbol || !it->relations()) {
        return;
    }

    for(auto rel: *it->relations()) {
        auto r = safe_cast<Relation>(rel);
        if(r->kind & kind) {
            if(!callback(*r)) {
                return;
            }
        }
    }
}

bool PreambleState::find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto& syms = *root->symbols();

    auto it = std::ranges::lower_bound(syms, hash, {}, [](auto e) { return e->symbol_id(); });
    if(it == syms.end() || it->symbol_id() != hash) {
        return false;
    }

    name = it->name() ? it->name()->str() : std::string();
    kind = SymbolKind(static_cast<std::uint8_t>(it->kind()));
    return true;
}

std::vector<feature::DocumentLink> PreambleState::links() const {
    std::vector<feature::DocumentLink> links;
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    if(auto ls = root->links()) {
        links.reserve(ls->size());
        for(auto entry: *ls) {
            feature::DocumentLink link;
            if(auto range = entry->range()) {
                link.range = *safe_cast<Range>(range);
            }
            if(auto target = entry->target()) {
                link.target = target->str();
            }
            links.push_back(std::move(link));
        }
    }
    return links;
}

llvm::ArrayRef<std::uint32_t> PreambleState::inactive_regions() const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto regions = root->inactive_regions();
    if(!regions) {
        return {};
    }
    return llvm::ArrayRef(regions->data(), regions->size());
}

llvm::ArrayRef<std::uint8_t> PreambleState::open_conditionals() const {
    auto root = fbs::GetRoot<binary::PreambleState>(buffer->getBufferStart());
    auto conditionals = root->open_conditionals();
    if(!conditionals) {
        return {};
    }
    return llvm::ArrayRef(conditionals->data(), conditionals->size());
}

}  // namespace clice::index
