#include "index/tu_index.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <tuple>

#include "index/kotatsu_adapters.h"  // type_adapter specializations
#include "semantic/ast_utility.h"
#include "semantic/semantic_visitor.h"

#include "kota/codec/flatbuffers/deserializer.h"
#include "kota/codec/flatbuffers/serializer.h"
#include "llvm/Support/SHA256.h"

namespace clice::index {

namespace {

namespace kfb = kota::codec::flatbuffers;

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

        auto interested = unit.interested_file();

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
        }

        // Populate main_file_index (interested file) and path_file_indices
        // (keyed by path_id) for serialization. `file_indices` itself is
        // `skip`-marked (runtime-only, keyed by clang::FileID) and retained
        // for in-memory consumers/tests that need FileID access.
        for(auto& [fid, index]: result.file_indices) {
            if(fid == interested) {
                result.main_file_index = index;
            } else {
                result.path_file_indices[result.graph.path_id(fid)] = index;
            }
        }
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

void TUIndex::serialize(llvm::raw_ostream& os) {
    auto bytes = kfb::to_flatbuffer(*this);
    assert(bytes && "TUIndex flatbuffer serialization failed");
    os.write(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

TUIndex TUIndex::from(const void* data, std::size_t size) {
    TUIndex index;
    if(data == nullptr || size == 0) {
        return index;
    }

    std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(data), size);
    auto result = kfb::from_flatbuffer(bytes, index);
    if(!result) {
        return TUIndex();
    }
    return index;
}

}  // namespace clice::index
