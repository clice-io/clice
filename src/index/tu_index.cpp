#include "index/tu_index.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <tuple>

#include "index/serialization.h"
#include "semantic/ast_utility.h"
#include "semantic/semantic_visitor.h"
#include "syntax/lexer.h"

#include "llvm/Support/SHA256.h"
#include "llvm/Support/xxhash.h"
#include "clang/AST/DeclCXX.h"

namespace clice::index {

namespace {

SymbolScope classify_scope(const clang::NamedDecl* decl) {
    auto linkage = decl->getFormalLinkage();
    if(linkage == clang::Linkage::None)
        return SymbolScope::FileLocal;
    if(linkage == clang::Linkage::Internal || linkage == clang::Linkage::Module)
        return SymbolScope::TULocal;
    return SymbolScope::External;
}

class Builder : public SemanticVisitor<Builder> {
public:
    Builder(TUIndex& result, CompilationUnitRef unit, bool interested_only) :
        SemanticVisitor<Builder>(unit, interested_only), result(result) {
        result.graph = IncludeGraph::from(unit);
    }

    /// Per-file index for `fid`: the interested file accumulates into
    /// main_file_index, every other file into its path-id slot (several
    /// FileIDs of one path merge into the same entry).
    FileIndex& index_of(clang::FileID fid) {
        if(fid == unit.interested_file()) {
            return result.main_file_index;
        }
        return result.path_file_indices[result.graph.path_id(fid)];
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
        auto& index = index_of(fid);

        auto symbol_id = unit.getSymbolID(decl);
        auto [it, success] = result.symbols.try_emplace(symbol_id.hash);
        if(success) {
            auto& symbol = it->second;
            symbol.name = ast::display_name_of(decl);
            symbol.kind = SymbolKind::from(decl);
            symbol.scope = classify_scope(decl);
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
        auto& index = index_of(fid);

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

        auto& index = index_of(fid);
        auto symbol_id = unit.getSymbolID(ast::normalize(decl));
        index.relations[symbol_id.hash].emplace_back(relation);
    }

    /// Module names are indexed like macro names: an occurrence plus a
    /// Definition/Reference relation keyed by a hash of the full module
    /// name, so navigation flows through the ordinary index pipeline.
    void index_modules() {
        auto emit = [&](llvm::StringRef name,
                        clang::FileID fid,
                        LocalSourceRange range,
                        RelationKind kind) {
            if(name.empty())
                return;
            if(interested_only && fid != unit.interested_file())
                return;
            llvm::SmallString<64> usr("@module@");
            usr += name;
            auto hash = llvm::xxh3_64bits(usr);

            auto& index = index_of(fid);
            index.occurrences.emplace_back(range, hash);
            Relation relation{
                .kind = kind,
                .range = range,
                .target_symbol = 0,
            };
            // Decl/def consumers read the definition range out of
            // target_symbol; without it, module symbols would report their
            // definition as missing.
            if(kind.isDeclOrDef()) {
                relation.set_definition_range(range);
            }
            index.relations[hash].emplace_back(relation);

            auto& symbol = result.symbols[hash];
            if(symbol.name.empty()) {
                symbol.name = name.str();
                symbol.kind = SymbolKind::Module;
                symbol.scope = SymbolScope::External;
            }
        };

        // Import sites: Reference relations at the spelled module name. The
        // expansion range keeps macro-spelled names (`import MOD;`) anchored
        // at the import site instead of the macro definition.
        for(auto& [fid, directive]: unit.directives()) {
            for(auto& import: directive.imports) {
                if(import.name_locations.empty())
                    continue;
                auto [loc_fid, range] = unit.decompose_expansion_range(
                    clang::SourceRange(import.name_locations.front(),
                                       import.name_locations.back()));
                llvm::StringRef name = import.full_name.empty() ? import.name : import.full_name;
                emit(name, loc_fid, range, RelationKind::Reference);
            }
        }

        // The module declaration of this unit: Definition in the interface
        // unit, Reference in an implementation unit. The declaration has no
        // AST node or PP location, so locate the name with the lexer.
        if(!unit.is_named_module()) {
            return;
        }
        auto module_name = unit.module_name();
        if(!module_name.empty()) {
            // interested_content() is the full, NUL-terminated buffer; the
            // lexer token ranges are offsets into it, i.e. file offsets.
            llvm::StringRef content = unit.interested_content();
            Lexer lexer(content);

            auto is_identifier = [](const Token& token) {
                return token.is_identifier();
            };

            bool found = false;
            std::uint32_t name_begin = 0;
            std::uint32_t name_end = 0;

            // Whether the previous token was `export` at the start of a line,
            // so a following `module` still introduces the declaration.
            bool after_export = false;

            while(true) {
                auto token = lexer.advance();
                if(token.is_eof())
                    break;

                // The `module` declaration keyword either starts the line or
                // follows an `export` that starts the line (`export module M;`).
                bool at_decl_start = token.is_at_start_of_line || after_export;
                after_export = token.is_at_start_of_line && token.is_identifier() &&
                               token.text(content) == "export";

                // Only interested in a `module` keyword whose next token is an
                // identifier (the name). This skips `module;` (global module
                // fragment, next is `;`) and `module :private;` (next is `:`).
                if(!at_decl_start || !token.is_identifier() || token.text(content) != "module")
                    continue;

                auto next = lexer.next();
                if(!next.is_identifier())
                    continue;

                auto first = lexer.advance_if(is_identifier);
                if(!first)
                    continue;
                name_begin = first->range.begin;
                name_end = first->range.end;
                while(true) {
                    auto sep = lexer.advance_if([](const Token& token) {
                        return token.kind == clang::tok::period || token.kind == clang::tok::colon;
                    });
                    if(!sep)
                        break;
                    auto part = lexer.advance_if(is_identifier);
                    if(!part)
                        break;
                    name_end = part->range.end;
                }
                found = true;
                break;
            }

            if(found) {
                emit(module_name,
                     unit.interested_file(),
                     LocalSourceRange{name_begin, name_end},
                     unit.is_module_interface_unit() ? RelationKind::Definition
                                                     : RelationKind::Reference);
            }
        }
    }

    void build() {
        run();

        index_modules();

        auto finalize = [&](std::uint32_t path_id, FileIndex& index) {
            for(auto& [symbol_id, relations]: index.relations) {
                std::ranges::sort(relations);
                auto dup = std::ranges::unique(relations);
                relations.erase(dup.begin(), dup.end());
                result.symbols[symbol_id].reference_files.add(path_id);
            }

            std::ranges::sort(index.occurrences);
            auto dup = std::ranges::unique(index.occurrences);
            index.occurrences.erase(dup.begin(), dup.end());
        };

        for(auto& [path_id, index]: result.path_file_indices) {
            finalize(path_id, index);
        }
        /// Main file is the last path in graph.paths (convention from IncludeGraph).
        finalize(static_cast<std::uint32_t>(result.graph.paths.size() - 1), result.main_file_index);
    }

private:
    TUIndex& result;
};

}  // namespace

void lookup_occurrences(std::span<const Occurrence> occurrences,
                        std::uint32_t offset,
                        llvm::function_ref<bool(const Occurrence&)> callback) {
    auto it = std::ranges::lower_bound(occurrences, offset, {}, [](const Occurrence& o) {
        return o.range.end;
    });
    while(it != occurrences.end() && it->range.contains(offset)) {
        if(!callback(*it))
            return;
        ++it;
    }
}

void FileIndex::lookup(std::uint32_t offset,
                       llvm::function_ref<bool(const Occurrence&)> callback) const {
    lookup_occurrences(occurrences, offset, callback);
}

void FileIndex::lookup(SymbolHash symbol,
                       RelationKind kind,
                       llvm::function_ref<bool(const Relation&)> callback) const {
    auto it = relations.find(symbol);
    if(it == relations.end())
        return;
    for(auto& r: it->second) {
        if(r.kind & kind) {
            if(!callback(r))
                return;
        }
    }
}

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
    write_flatbuffer(os, *this);
}

TUIndex TUIndex::from(const void* data, std::size_t size) {
    TUIndex index;
    if(!read_flatbuffer(data, size, index)) {
        return TUIndex();
    }
    return index;
}

}  // namespace clice::index
