#include "AST/Semantic.h"
#include "Index/TUIndex.h"
#include "Support/Compare.h"

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
        assert(decl && "Invalid decl");
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
            auto [fid2, definitionRange] = unit.decompose_expansion_range(decl->getSourceRange());
            assert(fid == fid2 && "Invalid definition location");
            relation.range = relationRange;
            relation.definition_range = definitionRange;
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
            std::ranges::sort(index.occurrences, refl::less);
        }
    }

private:
    TUIndex& result;
};

}  // namespace

TUIndex TUIndex::build(CompilationUnit& unit) {
    TUIndex index;
    Builder builder(index, unit);
    builder.build();
    return index;
}

}  // namespace clice::index
