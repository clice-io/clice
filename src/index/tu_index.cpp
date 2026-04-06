#include "index/tu_index.h"

#include <tuple>

#include "index/serialization.h"
#include "semantic/ast_utility.h"
#include "semantic/semantic_visitor.h"

#include "llvm/Support/SHA256.h"

namespace clice::index {

namespace {

class Builder : public SemanticVisitor<Builder> {
public:
    Builder(TUIndex& result, CompilationUnitRef unit, bool interested_only) :
        SemanticVisitor<Builder>(unit, interested_only), result(result) {
        result.graph = IncludeGraph::from(unit);
    }

    void handleDeclOccurrence(const clang::NamedDecl* decl,
                              RelationKind kind,
                              clang::SourceLocation location) {
        decl = ast::normalize(decl);

        if(location.isMacroID()) {
            auto spelling = unit.spelling_location(location);
            auto expansion = unit.expansion_location(location);

            /// FIXME: For location from macro, we only handle the case that the
            /// spelling and expansion are in the same file currently.
            if(unit.file_id(spelling) != unit.file_id(expansion)) {
                return;
            }

            /// For occurrence, we always use spelling location.
            location = spelling;
        }

        auto [fid, range] = unit.decompose_range(location);
        auto& index = result.file_indices[fid];

        auto symbol_id = unit.getSymbolID(decl);
        auto [it, success] = result.symbols.try_emplace(symbol_id.hash);
        if(success) {
            auto& symbol = it->second;
            symbol.name = ast::display_name_of(decl);
            symbol.kind = SymbolKind::from(decl);
        }
        index.occurrences.emplace_back(range, symbol_id.hash);
    }

    void handleMacroOccurrence(const clang::MacroInfo* def,
                               RelationKind kind,
                               clang::SourceLocation location) {
        /// FIXME: Figure out when location is MacroID.
        if(location.isMacroID()) {
            return;
        }

        auto [fid, range] = unit.decompose_range(location);
        auto& index = result.file_indices[fid];

        auto symbol_id = unit.getSymbolID(def);
        index.occurrences.emplace_back(range, symbol_id.hash);

        Relation relation{
            .kind = kind,
            .range = range,
            .target_symbol = 0,
        };

        index.relations[symbol_id.hash].emplace_back(relation);
    }

    void handleRelation(const clang::NamedDecl* decl,
                        RelationKind kind,
                        const clang::NamedDecl* target,
                        clang::SourceRange range) {
        auto [fid, relation_range] = unit.decompose_expansion_range(range);

        Relation relation{.kind = kind};

        if(kind.isDeclOrDef()) {
            relation.range = relation_range;
            /// FIXME: why definition or declaration has invalid source range? implicit node?
            auto source_range = decl->getSourceRange();
            if(source_range.isValid()) {
                auto [fid2, definition_range] =
                    unit.decompose_expansion_range(decl->getSourceRange());
                assert(fid == fid2 && "Invalid definition location");
                relation.set_definition_range(definition_range);
            }
        } else if(kind.isReference()) {
            relation.range = relation_range;
            relation.target_symbol = 0;
        } else if(kind.isBetweenSymbol()) {
            auto symbol_id = unit.getSymbolID(ast::normalize(target));
            relation.target_symbol = symbol_id.hash;
        } else if(kind.isCall()) {
            auto symbol_id = unit.getSymbolID(ast::normalize(target));
            relation.range = relation_range;
            relation.target_symbol = symbol_id.hash;
        } else {
            std::unreachable();
        }

        auto& index = result.file_indices[fid];
        auto symbol_id = unit.getSymbolID(ast::normalize(decl));
        index.relations[symbol_id.hash].emplace_back(relation);
    }

    void build() {
        run();

        for(auto& [fid, index]: result.file_indices) {
            for(auto& [symbol_id, relations]: index.relations) {
                std::ranges::sort(relations, [](const Relation& lhs, const Relation& rhs) {
                    return std::tuple(lhs.kind.value(),
                                      lhs.range.begin,
                                      lhs.range.end,
                                      lhs.target_symbol) < std::tuple(rhs.kind.value(),
                                                                      rhs.range.begin,
                                                                      rhs.range.end,
                                                                      rhs.target_symbol);
                });
                auto range =
                    std::ranges::unique(relations, [](const Relation& lhs, const Relation& rhs) {
                        return lhs.kind == rhs.kind && lhs.range == rhs.range &&
                               lhs.target_symbol == rhs.target_symbol;
                    });
                relations.erase(range.begin(), range.end());
                result.symbols[symbol_id].reference_files.add(result.graph.path_id(fid));
            }

            std::ranges::sort(index.occurrences, [](const Occurrence& lhs, const Occurrence& rhs) {
                return std::tuple(lhs.range.begin, lhs.range.end, lhs.target) <
                       std::tuple(rhs.range.begin, rhs.range.end, rhs.target);
            });
            auto range =
                std::ranges::unique(index.occurrences,
                                    [](const Occurrence& lhs, const Occurrence& rhs) {
                                        return lhs.range == rhs.range && lhs.target == rhs.target;
                                    });
            index.occurrences.erase(range.begin(), range.end());

            if(fid == unit.interested_file()) {
                result.main_file_index = std::move(index);
            }
        }

        result.file_indices.erase(unit.interested_file());
    }

private:
    TUIndex& result;
};

}  // namespace

std::array<std::uint8_t, 32> FileIndex::hash() {
    llvm::SHA256 hasher;

    using u8 = std::uint8_t;

    if(!occurrences.empty()) {
        static_assert(sizeof(Occurrence) == sizeof(Range) + sizeof(SymbolHash));
        static_assert(sizeof(Occurrence) % 8 == 0);
        auto data = reinterpret_cast<u8*>(occurrences.data());
        auto size = occurrences.size() * sizeof(Occurrence);
        hasher.update(llvm::ArrayRef(data, size));
    }

    for(auto& [symbol_id, relations]: relations) {
        hasher.update(std::bit_cast<std::array<u8, sizeof(symbol_id)>>(symbol_id));
        static_assert(sizeof(Relation) ==
                      sizeof(RelationKind) + 4 + sizeof(Range) + sizeof(SymbolHash));
        static_assert(sizeof(Relation) % 8 == 0);

        if(!relations.empty()) {
            auto data = reinterpret_cast<u8*>(relations.data());
            auto size = relations.size() * sizeof(Relation);
            hasher.update(llvm::ArrayRef(data, size));
        }
    }

    return hasher.final();
}

TUIndex TUIndex::build(CompilationUnitRef unit, bool interested_only) {
    TUIndex index;
    index.built_at = unit.build_at();

    Builder builder(index, unit, interested_only);
    builder.build();

    return index;
}

void TUIndex::serialize(llvm::raw_ostream& os) const {
    fbs::FlatBufferBuilder builder(4096);

    llvm::SmallVector<char, 1024> buffer;

    auto paths =
        transform(graph.paths, [&](const std::string& p) { return builder.CreateString(p); });

    auto syms = transform(symbols, [&](auto&& value) {
        auto& [symbol_id, symbol] = value;
        buffer.clear();
        buffer.resize_for_overwrite(symbol.reference_files.getSizeInBytes(false));
        symbol.reference_files.write(buffer.data(), false);
        return binary::CreateSymbolEntry(builder,
                                         symbol_id,
                                         binary::CreateSymbol(builder,
                                                              CreateString(builder, symbol.name),
                                                              symbol.kind.value(),
                                                              CreateVector(builder, buffer)));
    });

    /// Serialize a single FileIndex into a TUFileIndexEntry.
    auto serialize_file_index = [&](std::uint32_t fid, const FileIndex& index) {
        auto occs = CreateStructVector<binary::Occurrence>(builder, index.occurrences);
        auto rels = transform(index.relations, [&](auto&& value) {
            auto& [symbol_id, relations] = value;
            return binary::CreateTUFileRelationsEntry(
                builder,
                symbol_id,
                CreateStructVector<binary::Relation>(builder, relations));
        });
        return binary::CreateTUFileIndexEntry(builder, fid, occs, CreateVector(builder, rels));
    };

    /// Convert FileID-keyed file_indices to path_id-keyed entries.
    llvm::SmallVector<fbs::Offset<binary::TUFileIndexEntry>> file_idx_vec;
    for(auto& [fid, index]: file_indices) {
        auto pid = graph.path_id(fid);
        file_idx_vec.push_back(serialize_file_index(pid, index));
    }

    /// Main file is the last path in graph.paths (convention from IncludeGraph).
    auto main_idx =
        serialize_file_index(static_cast<std::uint32_t>(graph.paths.size() - 1), main_file_index);

    auto tu_index =
        binary::CreateTUIndex(builder,
                              static_cast<std::uint64_t>(built_at.count()),
                              CreateVector(builder, paths),
                              CreateStructVector<binary::IncludeLocation>(builder, graph.locations),
                              CreateVector(builder, syms),
                              builder.CreateVector(file_idx_vec.data(), file_idx_vec.size()),
                              main_idx);

    builder.Finish(tu_index);
    os.write(safe_cast<const char>(builder.GetBufferPointer()), builder.GetSize());
}

TUIndex TUIndex::from(const void* data) {
    auto root = fbs::GetRoot<binary::TUIndex>(data);

    TUIndex index;
    index.built_at = std::chrono::milliseconds(root->built_at());

    for(auto p: *root->paths()) {
        index.graph.paths.emplace_back(p->str());
    }

    for(auto loc: *root->locations()) {
        index.graph.locations.emplace_back(*safe_cast<IncludeLocation>(loc));
    }

    for(auto entry: *root->symbols()) {
        auto& symbol = index.symbols[entry->symbol_id()];
        symbol.name = entry->symbol()->name()->str();
        symbol.kind = SymbolKind(static_cast<std::uint8_t>(entry->symbol()->kind()));
        symbol.reference_files = read_bitmap(entry->symbol()->refs());
    }

    /// Helper to deserialize a TUFileIndexEntry into a FileIndex.
    auto deserialize_file_index = [](const binary::TUFileIndexEntry* entry) -> FileIndex {
        FileIndex fi;
        if(entry->occurrences()) {
            fi.occurrences.reserve(entry->occurrences()->size());
            for(auto o: *entry->occurrences()) {
                fi.occurrences.emplace_back(*safe_cast<Occurrence>(o));
            }
        }
        if(entry->relations()) {
            for(auto rel_entry: *entry->relations()) {
                auto& rels = fi.relations[rel_entry->symbol()];
                if(rel_entry->relations()) {
                    rels.reserve(rel_entry->relations()->size());
                    for(auto r: *rel_entry->relations()) {
                        rels.emplace_back(*safe_cast<Relation>(r));
                    }
                }
            }
        }
        return fi;
    };

    /// Populate path_file_indices keyed by path_id (no clang::FileID needed).
    if(root->file_indices()) {
        for(auto entry: *root->file_indices()) {
            index.path_file_indices[entry->file_id()] = deserialize_file_index(entry);
        }
    }

    if(root->main_file_index()) {
        index.main_file_index = deserialize_file_index(root->main_file_index());
    }

    return index;
}

}  // namespace clice::index
