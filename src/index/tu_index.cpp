#include "index/tu_index.h"

#include <algorithm>
#include <tuple>

#include "compile/compilation_unit.h"
#include "index/serialization.h"
#include "semantic/decls.h"
#include "semantic/display.h"
#include "semantic/semantics.h"
#include "semantic/types.h"

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

/// Projects the unit's semantic map into TUIndex rows: occurrences and
/// relations from the resolve facts, macros from the preprocessor directives.
class Projector {
public:
    Projector(TUIndex& result, CompilationUnitRef unit, bool interested_only) :
        result(result), unit(unit), interested_only(interested_only) {}

    /// The only gate through which rows enter `file_indices`. With
    /// interested_only, the index covers just the interested file — yet
    /// AST nodes reachable from its top-level decls can carry locations
    /// in other files: inherited default arguments and base specifiers
    /// of classes defined in a preamble header land there. Such rows
    /// belong to the preamble's own index, so they are dropped here.
    FileIndex* file_index(clang::FileID fid) {
        if(interested_only && fid != unit.interested_file()) {
            return nullptr;
        }
        return &result.file_indices[fid];
    }

    void add_occurrence(const clang::NamedDecl* decl,
                        RelationKind kind,
                        clang::SourceLocation location) {
        decl = decls::normalize(decl);

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
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        auto symbol_id = unit.getSymbolID(decl);
        auto [it, success] = result.symbols.try_emplace(symbol_id.hash);
        if(success) {
            auto& symbol = it->second;
            symbol.name = display::name_of(decl);
            symbol.kind = SymbolKind::from(decl);
            symbol.scope = classify_scope(decl);
        }
        index->occurrences.emplace_back(range, symbol_id.hash);
    }

    void add_macro(const clang::MacroInfo* def, RelationKind kind, clang::SourceLocation location) {
        /// FIXME: Figure out when location is MacroID.
        if(location.isMacroID()) {
            return;
        }

        auto [fid, range] = unit.decompose_range(location);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        auto symbol_id = unit.getSymbolID(def);
        // Macros get a symbol-table entry like declarations do; without it
        // build() would default-construct a nameless entry when recording
        // reference files, and every name lookup for the macro would come
        // back empty.
        auto [it, success] = result.symbols.try_emplace(symbol_id.hash);
        if(success) {
            auto& symbol = it->second;
            symbol.name = unit.token_spelling(location).str();
            symbol.kind = SymbolKind::Macro;
            symbol.scope = SymbolScope::External;
        }
        index->occurrences.emplace_back(range, symbol_id.hash);

        Relation relation{
            .kind = kind,
            .range = range,
            .target_symbol = 0,
        };

        // Definition relations carry the macro's full extent (name through
        // last body token), like declarations do — definition-text
        // consumers decode it out of target_symbol.
        if(kind.isDeclOrDef() && def) {
            auto [def_fid, def_range] = unit.decompose_range(
                clang::SourceRange(def->getDefinitionLoc(), def->getDefinitionEndLoc()));
            if(def_fid == fid) {
                relation.set_definition_range(def_range);
            }
        }

        index->relations[symbol_id.hash].emplace_back(relation);
    }

    /// A Definition/Declaration/Reference row mirroring an occurrence: it
    /// lands at the name's expansion location, and decl/def rows also carry
    /// the declaration's full extent for definition-text consumers.
    void add_self_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           clang::SourceLocation location) {
        auto [fid, range] = unit.decompose_expansion_range(location);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        Relation relation{.kind = kind, .range = range, .target_symbol = 0};

        if(kind.isDeclOrDef()) {
            /// FIXME: why definition or declaration has invalid source range? implicit node?
            auto source_range = decl->getSourceRange();
            if(source_range.isValid()) {
                auto [def_fid, definition_range] = unit.decompose_expansion_range(source_range);
                /// A declaration can begin in another file, e.g. when a
                /// header-defined macro spells its leading tokens. Such a
                /// range is meaningless in this file's coordinates; leave
                /// the definition range empty instead of storing it.
                if(fid == def_fid) {
                    relation.set_definition_range(definition_range);
                }
            }
        }

        index->relations[unit.getSymbolID(decls::normalize(decl)).hash].emplace_back(relation);
    }

    /// A symbol-to-symbol row (type-of, inheritance, overrides, ctor/dtor
    /// ownership); `anchor` decides which file's index receives it.
    void add_pair_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           const clang::NamedDecl* target,
                           clang::SourceRange anchor) {
        /// The anchor only routes the row to a file's index; symbol pairs
        /// carry no range of their own.
        auto fid = unit.decompose_expansion_range(anchor).first;
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        Relation relation{
            .kind = kind,
            .target_symbol = unit.getSymbolID(decls::normalize(target)).hash,
        };
        index->relations[unit.getSymbolID(decls::normalize(decl)).hash].emplace_back(relation);
    }

    /// A call edge, landing at the call expression's location.
    void add_call_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           const clang::NamedDecl* target,
                           clang::SourceRange range) {
        auto [fid, relation_range] = unit.decompose_expansion_range(range);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        Relation relation{
            .kind = kind,
            .range = relation_range,
            .target_symbol = unit.getSymbolID(decls::normalize(target)).hash,
        };
        index->relations[unit.getSymbolID(decls::normalize(decl)).hash].emplace_back(relation);
    }

    /// Module names are indexed like macro names: an occurrence plus a
    /// Definition/Reference relation keyed by a hash of the full module
    /// name, so navigation flows through the ordinary index pipeline.
    void index_modules(const Semantics& semantics) {
        auto emit = [&](llvm::StringRef name,
                        clang::FileID fid,
                        LocalSourceRange range,
                        RelationKind kind) {
            if(name.empty())
                return;
            auto* index = file_index(fid);
            if(!index)
                return;
            llvm::SmallString<64> usr("@module@");
            usr += name;
            auto hash = llvm::xxh3_64bits(usr);

            index->occurrences.emplace_back(range, hash);
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
            index->relations[hash].emplace_back(relation);

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
        // AST node or PP location; the semantics' lexical scan located and
        // cross-checked its written tokens. The occurrence spans the written
        // name, partition included.
        if(!unit.is_named_module()) {
            return;
        }
        auto module_name = unit.module_name();
        if(module_name.empty()) {
            return;
        }
        for(auto& module: semantics.module_declarations()) {
            if(module.kind != LexicalInfo::ModuleDeclaration::Kind::Declaration) {
                continue;
            }
            auto name_begin = module.name_parts.front().begin;
            auto name_end = (module.partition_parts.empty() ? module.name_parts.back()
                                                            : module.partition_parts.back())
                                .end;
            emit(module_name,
                 unit.interested_file(),
                 LocalSourceRange{name_begin, name_end},
                 unit.is_module_interface_unit() ? RelationKind::Definition
                                                 : RelationKind::Reference);
            break;
        }
    }

    /// The nearest enclosing function of node `index`, for call edges. Methods
    /// count as callers too (the previous traversal-stack implementation
    /// missed them: methods take a different RAV path than TraverseFunctionDecl).
    ///
    /// Memoized with path compression: calls nested under a deep expression
    /// chain would otherwise each rescan thousands of the same ancestors.
    const clang::NamedDecl* enclosing_function(const Semantics& semantics, std::uint32_t index) {
        llvm::SmallVector<std::uint32_t> path;
        const clang::NamedDecl* result = nullptr;

        for(auto p = semantics.node(index).parent; p != Semantics::invalid;
            p = semantics.node(p).parent) {
            if(auto it = enclosing_cache.find(p); it != enclosing_cache.end()) {
                result = it->second;
                break;
            }
            if(auto* decl = semantics.node(p).node.get<clang::FunctionDecl>()) {
                result = decl;
                break;
            }
            path.push_back(p);
        }

        for(auto p: path) {
            enclosing_cache.try_emplace(p, result);
        }
        return result;
    }

    /// Decl-pair relation facts: type definitions, inheritance, overrides,
    /// constructor/destructor ownership and call edges. Only the index
    /// consumes these, so they live here rather than in the semantic layer.
    void project_relations(const Semantics& semantics, std::uint32_t index) {
        const SemanticNode& node = semantics.node(index).node;

        if(auto* CE = node.get<clang::CallExpr>()) {
            const clang::NamedDecl* caller = enclosing_function(semantics, index);
            const clang::NamedDecl* callee =
                llvm::dyn_cast_if_present<clang::NamedDecl>(CE->getCalleeDecl());
            if(caller && callee) {
                add_call_relation(caller, RelationKind::Callee, callee, CE->getSourceRange());
                add_call_relation(callee, RelationKind::Caller, caller, CE->getSourceRange());
            }
            return;
        }

        auto* D = node.get<clang::Decl>();
        if(!D) {
            return;
        }

        /// The type of a value declaration, for go-to-type-definition.
        if(llvm::isa<clang::FieldDecl,
                     clang::BindingDecl,
                     clang::NonTypeTemplateParmDecl,
                     clang::VarDecl>(D)) {
            if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(D)) {
                switch(VTSD->getSpecializationKind()) {
                    case clang::TSK_ImplicitInstantiation:
                    case clang::TSK_ExplicitInstantiationDeclaration:
                    case clang::TSK_ExplicitInstantiationDefinition: {
                        return;
                    }

                    case clang::TSK_Undeclared:
                    case clang::TSK_ExplicitSpecialization: {
                        break;
                    }
                }
            }

            auto* VD = llvm::cast<clang::ValueDecl>(D);
            if(auto target = types::decl_of(VD->getType())) {
                add_pair_relation(VD, RelationKind::TypeDefinition, target, VD->getLocation());
            }
            return;
        }

        if(auto* ECD = llvm::dyn_cast<clang::EnumConstantDecl>(D)) {
            add_pair_relation(ECD,
                              RelationKind::TypeDefinition,
                              llvm::cast<clang::NamedDecl>(ECD->getDeclContext()),
                              ECD->getLocation());
            return;
        }

        if(auto* TND = llvm::dyn_cast<clang::TypedefNameDecl>(D)) {
            if(auto target = types::decl_of(TND->getUnderlyingType())) {
                add_pair_relation(TND, RelationKind::TypeDefinition, target, TND->getLocation());
            }
            return;
        }

        /// Base/derived edges, recorded at the defining declaration.
        if(auto* TD = llvm::dyn_cast<clang::TagDecl>(D)) {
            if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(TD)) {
                switch(CTSD->getSpecializationKind()) {
                    case clang::TSK_Undeclared:
                    case clang::TSK_ImplicitInstantiation:
                    case clang::TSK_ExplicitInstantiationDeclaration:
                    case clang::TSK_ExplicitInstantiationDefinition: {
                        return;
                    }

                    case clang::TSK_ExplicitSpecialization: {
                        break;
                    }
                }
            }

            if(auto* CRD = llvm::dyn_cast<clang::CXXRecordDecl>(TD)) {
                if(auto* def = CRD->getDefinition()) {
                    for(auto& base: CRD->bases()) {
                        /// FIXME: Handle dependent base class.
                        if(auto target = types::decl_of(base.getType())) {
                            add_pair_relation(def,
                                              RelationKind::Base,
                                              target,
                                              base.getSourceRange());
                            add_pair_relation(target,
                                              RelationKind::Derived,
                                              def,
                                              base.getSourceRange());
                        }
                    }
                }
            }
            return;
        }

        if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(D)) {
            switch(FD->getTemplateSpecializationKind()) {
                case clang::TSK_ImplicitInstantiation:
                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    return;
                }

                case clang::TSK_Undeclared:
                case clang::TSK_ExplicitSpecialization: {
                    break;
                }
            }

            if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(FD)) {
                for(auto* base: method->overridden_methods()) {
                    add_pair_relation(method, RelationKind::Interface, base, FD->getLocation());
                    add_pair_relation(base,
                                      RelationKind::Implementation,
                                      method,
                                      FD->getLocation());
                }

                if(auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(method)) {
                    add_pair_relation(ctor,
                                      RelationKind::TypeDefinition,
                                      ctor->getParent(),
                                      FD->getLocation());
                    add_pair_relation(ctor->getParent(),
                                      RelationKind::Constructor,
                                      ctor,
                                      FD->getLocation());
                }

                if(auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(method)) {
                    add_pair_relation(dtor,
                                      RelationKind::TypeDefinition,
                                      dtor->getParent(),
                                      FD->getLocation());
                    add_pair_relation(dtor->getParent(),
                                      RelationKind::Destructor,
                                      dtor,
                                      FD->getLocation());
                }
            }
            return;
        }
    }

    void project_semantics(const Semantics& semantics) {
        auto entries = semantics.node_entries();

        for(std::uint32_t i = 0; i < entries.size(); i++) {
            const SemanticNode& node = entries[i].node;
            /// Macros are projected from the directives below; includes and
            /// imports have their own pipelines.
            if(!node.is_ast()) {
                continue;
            }

            for(auto& occurrence: resolve_occurrences(semantics, i, &unit.resolver())) {
                add_occurrence(occurrence.decl, occurrence.kind, occurrence.location);

                /// Every occurrence is mirrored as a self-relation, so
                /// find-references on the occurring decl finds this row.
                add_self_relation(occurrence.decl, occurrence.kind, occurrence.location);
            }

            project_relations(semantics, i);
        }

        for(auto& [fid, directive]: unit.directives()) {
            for(auto& macro: directive.macros) {
                switch(macro.kind) {
                    case MacroRef::Kind::Def: {
                        add_macro(macro.macro, RelationKind::Definition, macro.loc);
                        break;
                    }

                    case MacroRef::Kind::Ref:
                    case MacroRef::Kind::Undef: {
                        add_macro(macro.macro, RelationKind::Reference, macro.loc);
                        break;
                    }
                }
            }
        }
    }

    void build() {
        /// The interested-only shape is the one features share, cached on the
        /// unit; the whole-TU shape is transient — projected and dropped.
        /// Both phases below share the one build.
        std::optional<Semantics> full;
        if(!interested_only) {
            full.emplace(Semantics::build(unit, false));
        }
        const Semantics& semantics = interested_only ? unit.semantics() : *full;

        project_semantics(semantics);

        index_modules(semantics);

        // Build the include graph from what the index actually recorded:
        // every fid keying `file_indices` gets its include chain resolved
        // through the SourceManager, so the lookups below cannot miss.
        llvm::SmallVector<clang::FileID, 16> indexed_fids;
        indexed_fids.reserve(result.file_indices.size());
        for(auto& [fid, index]: result.file_indices) {
            indexed_fids.push_back(fid);
        }
        result.graph = IncludeGraph::from(unit, indexed_fids);

        for(auto& [fid, index]: result.file_indices) {
            for(auto& [symbol_id, relations]: index.relations) {
                std::ranges::sort(relations, [](const Relation& lhs, const Relation& rhs) {
                    return std::tuple(lhs.kind, lhs.range.begin, lhs.range.end, lhs.target_symbol) <
                           std::tuple(rhs.kind, rhs.range.begin, rhs.range.end, rhs.target_symbol);
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
    CompilationUnitRef unit;
    bool interested_only;
    llvm::DenseMap<std::uint32_t, const clang::NamedDecl*> enclosing_cache;
};

}  // namespace

void FileIndex::lookup(std::uint32_t offset,
                       llvm::function_ref<bool(const Occurrence&)> callback) const {
    auto it = std::ranges::lower_bound(occurrences, offset, {}, [](const Occurrence& o) {
        return o.range.end;
    });
    while(it != occurrences.end() && it->range.contains(offset)) {
        if(!callback(*it))
            return;
        ++it;
    }
}

void FileIndex::lookup(SymbolHash symbol,
                       RelationKind kind,
                       llvm::function_ref<bool(const Relation&)> callback) const {
    auto it = relations.find(symbol);
    if(it == relations.end())
        return;
    for(auto& r: it->second) {
        if(RelationKind(r.kind) & kind) {
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

    Projector projector(index, unit, interested_only);
    projector.build();

    return index;
}

void TUIndex::serialize(llvm::raw_ostream& os) {
    format_version = index_format_version;

    /// Convert the FileID-keyed working state into the persisted
    /// path_id-keyed form; multiple FileIDs can share a path id (repeated
    /// header contexts), last-wins. A deserialized index has no FileID-keyed
    /// state at all — its path-keyed rows already are the persisted form, so
    /// re-serializing must not wipe them.
    if(!file_indices.empty()) {
        path_file_indices.clear();
        for(auto& [fid, file_index]: file_indices) {
            path_file_indices[graph.path_id(fid)] = file_index;
        }
    }

    serialize_blob(*this, os);
}

std::optional<TUIndex> TUIndex::from(llvm::StringRef data) {
    std::optional<TUIndex> index{std::in_place};
    if(!deserialize_blob(data, *index) || index->format_version != index_format_version) {
        return std::nullopt;
    }
    // The verifier checks structure, not cross-field consistency: consumers
    // index path_hashes by path id, so normalize its length to the path
    // table's (absent hashes read as 0 = "unavailable").
    index->graph.path_hashes.resize(index->graph.paths.size(), 0);

    // Nor does it constrain field values, and every decoded path id is
    // dereferenced against the path table without further checks — graph
    // locations and per-file rows in Indexer::merge, reference_files through
    // ProjectIndex::merge's file_ids_map. A blob carrying an out-of-range
    // one is rejected as a whole.
    auto in_range = [count = index->graph.paths.size()](std::uint32_t path_id) {
        return path_id < count;
    };
    for(auto& location: index->graph.locations) {
        if(!in_range(location.path_id)) {
            return std::nullopt;
        }
    }
    for(auto& [path_id, _]: index->path_file_indices) {
        if(!in_range(path_id)) {
            return std::nullopt;
        }
    }
    for(auto& [_, symbol]: index->symbols) {
        if(!symbol.reference_files.isEmpty() && !in_range(symbol.reference_files.maximum())) {
            return std::nullopt;
        }
    }
    return index;
}

}  // namespace clice::index
