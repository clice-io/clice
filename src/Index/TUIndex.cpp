#include "Index/TUIndex.h"

#include "AST/Semantic.h"
#include "Support/Compare.h"

#include "llvm/Support/SHA256.h"

namespace clice::index {

namespace {

class Builder : public SemanticVisitor<Builder> {
public:
    Builder(TUIndex& result, CompilationUnit& unit) :
        SemanticVisitor<Builder>(unit, false), result(result) {
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
            .kind = RelationKind::Definition,
            .range = range,
            .target_symbol = 0,
        };

        index.relations[symbol_id.hash].emplace_back(relation);
    }

    void handleRelation(const clang::NamedDecl* decl,
                        RelationKind kind,
                        const clang::NamedDecl* target,
                        clang::SourceRange range) {
        auto [fid, relationRange] = unit.decompose_expansion_range(range);

        Relation relation{.kind = kind};

        if(kind.isDeclOrDef()) {
            auto [fid2, definition_range] = unit.decompose_expansion_range(decl->getSourceRange());
            assert(fid == fid2 && "Invalid definition location");
            relation.range = relationRange;
            relation.set_definition_range(definition_range);
        } else if(kind.isReference()) {
            relation.range = relationRange;
            relation.target_symbol = 0;
        } else if(kind.isBetweenSymbol()) {
            auto symbol_id = unit.getSymbolID(ast::normalize(target));
            relation.target_symbol = symbol_id.hash;
        } else if(kind.isCall()) {
            auto symbol_id = unit.getSymbolID(ast::normalize(target));
            relation.range = relationRange;
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
                std::ranges::sort(relations, refl::less);
                auto range = std::ranges::unique(relations, refl::equal);
                relations.erase(range.begin(), range.end());
                result.symbols[symbol_id].reference_files.add(result.graph.path_id(fid));
            }

            std::ranges::sort(index.occurrences, refl::less);
            auto range = std::ranges::unique(index.occurrences, refl::equal);
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

TUIndex TUIndex::build(CompilationUnit& unit) {
    TUIndex index;
    index.built_at = unit.build_at();

    Builder builder(index, unit);
    builder.build();

    return index;
}

}  // namespace clice::index
