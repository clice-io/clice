#include "index/tu_index.h"

#include <algorithm>
#include <tuple>

#include "compile/compilation_unit.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "semantic/decls.h"
#include "semantic/display.h"
#include "semantic/semantics.h"
#include "semantic/types.h"
#include "support/logging.h"
#include "support/timer.h"

#include "llvm/Support/xxhash.h"
#include "clang/AST/DeclCXX.h"

namespace clice::index {

namespace {

/// One file's rows on the wire: a self-contained single-variant shard
/// blob (index/shard.h). `hash` is xxh3 of `blob` — the variant's
/// identity — so the master can skip blobs it already stores without
/// touching their bytes.
struct FileSection {
    std::uint32_t path_id = 0;

    std::uint64_t hash = 0;

    std::vector<std::uint8_t> blob;
};

/// The envelope's wire layout. Only the builder below ever materializes
/// it; every consumer reads the bytes through the TUIndex reader.
struct EnvelopeBlob {
    /// Wire schema version (index_format_version), gated by
    /// TUIndex::from_bytes.
    /// A worker respawned after the binary on disk changed can be one
    /// build ahead of the server, and a layout change need not be
    /// structurally detectable.
    std::uint32_t format_version = 0;

    /// Milliseconds since epoch, sampled before the build started.
    std::int64_t built_at = 0;

    /// The include graph (IncludeGraph's persisted vectors): the path
    /// table, the consumed-content hash per path, and every include edge
    /// of the parse.
    std::vector<std::string> paths;
    std::vector<std::uint64_t> path_hashes;
    std::vector<IncludeLocation> locations;

    SymbolTable symbols;

    /// One entry per file with rows, ascending by path id.
    std::vector<FileSection> sections;

    /// Preamble ride-alongs, empty on ordinary envelopes: identity of the
    /// exact preamble text the PCH was built from (matches_prefix), and
    /// the PCH-derived feature state spliced into main-file results. The
    /// refs borrow the builder's inputs — encode-only, like the section
    /// blobs are for readers.
    std::uint64_t preamble_hash = 0;
    std::uint32_t preamble_size = 0;
    llvm::ArrayRef<feature::DocumentLink> links;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    llvm::ArrayRef<std::uint8_t> open_conditionals;
};

/// What build_preamble_index adds on top of an ordinary build.
struct PreambleExtras {
    std::uint64_t hash = 0;
    std::uint32_t size = 0;
    llvm::ArrayRef<feature::DocumentLink> links;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    llvm::ArrayRef<std::uint8_t> open_conditionals;
};

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
    Projector(CompilationUnitRef unit, bool interested_only) :
        unit(unit), interested_only(interested_only) {}

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
        return &file_indices[fid];
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
        auto [it, success] = symbols.try_emplace(symbol_id.hash);
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
        auto [it, success] = symbols.try_emplace(symbol_id.hash);
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

            auto& symbol = symbols[hash];
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

    std::string build(const PreambleExtras* extras) {
        ScopedTimer semantics_timer;
        /// The interested-only shape is the one features share, cached on the
        /// unit; the whole-TU shape is transient — projected and dropped.
        /// Both phases below share the one build.
        std::optional<Semantics> full;
        if(!interested_only) {
            full.emplace(Semantics::build(unit, false));
        }
        const Semantics& semantics = interested_only ? unit.semantics() : *full;
        auto semantics_ms = semantics_timer.ms_f();

        ScopedTimer project_timer;
        project_semantics(semantics);

        index_modules(semantics);
        auto project_ms = project_timer.ms_f();

        ScopedTimer finish_timer;
        // Build the include graph from what the index actually recorded:
        // every fid keying `file_indices` gets its include chain resolved
        // through the SourceManager, so the lookups below cannot miss.
        llvm::SmallVector<clang::FileID, 16> indexed_fids;
        indexed_fids.reserve(file_indices.size());
        for(auto& [fid, index]: file_indices) {
            indexed_fids.push_back(fid);
        }
        graph = IncludeGraph::from(unit, indexed_fids);

        for(auto& [fid, index]: file_indices) {
            for(auto symbol_id: llvm::make_first_range(index.relations)) {
                symbols[symbol_id].reference_files.add(graph.path_id(fid));
            }
        }
        auto finish_ms = finish_timer.ms_f();

        // Encode one blob per path. A header entered several times (its
        // FileIDs differ, its path id does not) contributes the union of
        // its entries' rows: write_shard canonicalizes — sorts and
        // deduplicates — so concatenation is union.
        ScopedTimer encode_timer;
        llvm::DenseMap<std::uint32_t, FileIndex> by_path;
        llvm::DenseMap<std::uint32_t, clang::FileID> path_fids;
        for(auto& [fid, index]: file_indices) {
            // A file with no include edge is a synthetic buffer (predefines,
            // <command line>): it has no real path to attribute rows to, and
            // path_id() would misfile them under the source file. Real files
            // forced in via -include are not affected — clang records their
            // include edge in the predefines buffer, which is a valid
            // location. The interested file legitimately has no edge.
            if(fid != unit.interested_file() &&
               graph.include_location_id(fid) == static_cast<std::uint32_t>(-1)) {
                continue;
            }
            auto path_id = graph.path_id(fid);
            path_fids.try_emplace(path_id, fid);
            auto& into = by_path[path_id];
            if(into.empty()) {
                into = std::move(index);
                continue;
            }
            into.occurrences.insert(into.occurrences.end(),
                                    index.occurrences.begin(),
                                    index.occurrences.end());
            for(auto& [hash, relations]: index.relations) {
                auto& group = into.relations[hash];
                group.insert(group.end(), relations.begin(), relations.end());
            }
        }

        auto resolve = [&](SymbolHash hash) -> std::optional<SymbolIdentity> {
            auto it = symbols.find(hash);
            if(it == symbols.end()) {
                return std::nullopt;
            }
            return SymbolIdentity{it->second.name, it->second.kind, it->second.scope};
        };

        llvm::SmallVector<std::uint32_t> path_ids;
        path_ids.reserve(by_path.size());
        for(auto path_id: llvm::make_first_range(by_path)) {
            path_ids.push_back(path_id);
        }
        llvm::sort(path_ids);
        std::vector<FileSection> sections;
        for(auto path_id: path_ids) {
            auto& rows = by_path[path_id];
            if(rows.empty()) {
                continue;
            }
            std::string bytes;
            llvm::raw_string_ostream os(bytes);
            write_shard(rows, resolve, unit.file_content(path_fids[path_id]), os);
            auto hash = llvm::xxh3_64bits(bytes);
            sections.push_back(
                {path_id, hash, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
        }
        auto encode_ms = encode_timer.ms_f();

        EnvelopeBlob blob;
        blob.format_version = index_format_version;
        blob.built_at = unit.build_at().count();
        blob.paths = std::move(graph.paths);
        blob.path_hashes = std::move(graph.path_hashes);
        blob.locations = std::move(graph.locations);
        blob.symbols = std::move(symbols);
        blob.sections = std::move(sections);
        if(extras) {
            blob.preamble_hash = extras->hash;
            blob.preamble_size = extras->size;
            blob.links = extras->links;
            blob.inactive_regions = extras->inactive_regions;
            blob.open_conditionals = extras->open_conditionals;
        }

        ScopedTimer pack_timer;
        std::string envelope;
        llvm::raw_string_ostream os(envelope);
        serialize_blob(blob, os);

        LOG_PERF("index_detail",
                 "op=build scope={} semantics_ms={:.2f} project_ms={:.2f} finish_ms={:.2f} "
                 "encode_ms={:.2f} pack_ms={:.2f}",
                 interested_only ? "interested" : "full",
                 semantics_ms,
                 project_ms,
                 finish_ms,
                 encode_ms,
                 pack_timer.ms_f());
        return envelope;
    }

private:
    CompilationUnitRef unit;
    bool interested_only;
    IncludeGraph graph;
    SymbolTable symbols;
    /// Build-time working state keyed by FileID — clang::FileID means
    /// nothing outside the compilation, so it never leaves the builder;
    /// the encode step converts it through graph.path_id.
    llvm::DenseMap<clang::FileID, FileIndex> file_indices;
    llvm::DenseMap<std::uint32_t, const clang::NamedDecl*> enclosing_cache;
};

}  // namespace

std::string build_tu_index(CompilationUnitRef unit, bool interested_only) {
    Projector projector(unit, interested_only);
    return projector.build(nullptr);
}

std::string build_preamble_index(CompilationUnitRef unit,
                                 llvm::ArrayRef<feature::DocumentLink> links,
                                 llvm::ArrayRef<std::uint32_t> inactive_regions,
                                 llvm::ArrayRef<std::uint8_t> open_conditionals) {
    // The preamble compile remaps the buffer truncated at the bound, so
    // interested_content() is exactly the preamble text the PCH was built
    // from.
    auto preamble_text = unit.interested_content();
    PreambleExtras extras{
        .hash = llvm::xxh3_64bits(preamble_text),
        .size = static_cast<std::uint32_t>(preamble_text.size()),
        .links = links,
        .inactive_regions = inactive_regions,
        .open_conditionals = open_conditionals,
    };
    Projector projector(unit, false);
    return projector.build(&extras);
}

namespace {

using WireView = kota::codec::fbs::table_view<EnvelopeBlob>;

/// The buffer was fully verified at TUIndex::from_bytes; per-accessor
/// views skip that cost.
WireView wire_root(llvm::StringRef data) {
    return WireView::from_verified_bytes(blob_bytes(data));
}

SymbolIdentity identity_of(kota::codec::fbs::table_view<Symbol> symbol) {
    return {to_ref(symbol[&Symbol::name]),
            SymbolKind(symbol[&Symbol::kind]),
            symbol[&Symbol::scope]};
}

/// The symbol's serialized reference bitmap (the Bitmap repr's byte image)
/// as a StringRef borrowing the wire.
llvm::StringRef bitmap_bytes(kota::codec::fbs::table_view<Symbol> symbol) {
    const auto* raw = symbol[&Symbol::reference_files].raw();
    if(!raw) {
        return {};
    }
    return llvm::StringRef(reinterpret_cast<const char*>(raw->data()), raw->size());
}

}  // namespace

TUIndex TUIndex::from_bytes(llvm::StringRef data) {
    auto root = WireView::from_bytes(blob_bytes(data));
    if(!root.valid() || root[&EnvelopeBlob::format_version] != index_format_version) {
        return {};
    }

    // Structural verification does not constrain field values; every path
    // id the merge dereferences against the path table is bounded here so
    // the accessors stay check-free. The builder ends every path table
    // with the interested file, so consumers address path_count() - 1
    // unchecked — an empty table marks a corrupt envelope.
    auto count = root[&EnvelopeBlob::paths].size();
    if(count == 0) {
        return {};
    }
    auto locations = root[&EnvelopeBlob::locations];
    for(std::size_t i = 0; i < locations.size(); i += 1) {
        IncludeLocation location = locations.at(i);
        if(location.path_id >= count) {
            return {};
        }
    }
    // section_of binary-searches the section table by path id and shard_of
    // trusts the result, so the ids must ascend strictly — a repeated or
    // out-of-order id would attribute one file's rows to another.
    auto sections = root[&EnvelopeBlob::sections];
    std::uint32_t previous_path_id = 0;
    for(std::size_t i = 0; i < sections.size(); i += 1) {
        auto path_id = sections.at(i)[&FileSection::path_id];
        if(path_id >= count || (i != 0 && path_id <= previous_path_id)) {
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
    return loaded() ? wire_root(data)[&EnvelopeBlob::built_at] : 0;
}

std::uint32_t TUIndex::path_count() const {
    return loaded() ? static_cast<std::uint32_t>(wire_root(data)[&EnvelopeBlob::paths].size()) : 0;
}

llvm::StringRef TUIndex::path(std::uint32_t id) const {
    return to_ref(wire_root(data)[&EnvelopeBlob::paths].at(id));
}

std::uint64_t TUIndex::path_hash(std::uint32_t id) const {
    // The hash column may be shorter than the path table on a foreign
    // blob; an absent hash reads as 0, "unavailable".
    auto hashes = wire_root(data)[&EnvelopeBlob::path_hashes];
    return id < hashes.size() ? hashes.at(id) : 0;
}

std::uint32_t TUIndex::location_count() const {
    return loaded() ? static_cast<std::uint32_t>(wire_root(data)[&EnvelopeBlob::locations].size())
                    : 0;
}

IncludeLocation TUIndex::location(std::uint32_t i) const {
    return wire_root(data)[&EnvelopeBlob::locations].at(i);
}

std::uint32_t TUIndex::section_count() const {
    return loaded() ? static_cast<std::uint32_t>(wire_root(data)[&EnvelopeBlob::sections].size())
                    : 0;
}

std::uint32_t TUIndex::section_path(std::uint32_t i) const {
    return wire_root(data)[&EnvelopeBlob::sections].at(i)[&FileSection::path_id];
}

std::uint64_t TUIndex::section_hash(std::uint32_t i) const {
    return wire_root(data)[&EnvelopeBlob::sections].at(i)[&FileSection::hash];
}

llvm::StringRef TUIndex::section_blob(std::uint32_t i) const {
    auto blob = to_array_ref(wire_root(data)[&EnvelopeBlob::sections].at(i)[&FileSection::blob]);
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

std::optional<std::uint32_t> TUIndex::section_of(std::uint32_t path_id) const {
    std::uint32_t lo = 0;
    std::uint32_t hi = section_count();
    while(lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if(section_path(mid) < path_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if(lo < section_count() && section_path(lo) == path_id) {
        return lo;
    }
    return std::nullopt;
}

const Shard& TUIndex::shard_of(std::uint32_t path_id) const {
    const static Shard missing;
    auto section = section_of(path_id);
    if(!section) {
        return missing;
    }
    if(shards.empty()) {
        shards.resize(section_count());
    }
    auto& slot = shards[*section];
    if(!slot.loaded()) {
        slot = Shard::from_bytes(section_blob(*section));
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
        // only the byte hash catches those, so a persisted envelope must
        // fail here and rebuild instead of serving corrupted rows.
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

void TUIndex::iterate_symbols(
    llvm::function_ref<bool(SymbolHash, const SymbolIdentity&, llvm::StringRef)> callback) const {
    if(!loaded()) {
        return;
    }
    auto symbols = wire_root(data)[&EnvelopeBlob::symbols];
    for(std::size_t i = 0; i < symbols.size(); i += 1) {
        auto entry = symbols.at(i);
        if(!callback(entry.get<0>(), identity_of(entry.get<1>()), bitmap_bytes(entry.get<1>()))) {
            return;
        }
    }
}

std::optional<SymbolIdentity> TUIndex::find_symbol(SymbolHash hash) const {
    if(!loaded()) {
        return std::nullopt;
    }
    auto found = wire_root(data)[&EnvelopeBlob::symbols].find(hash);
    if(!found) {
        return std::nullopt;
    }
    return identity_of(found->get<1>());
}

bool TUIndex::matches_prefix(llvm::StringRef text) const {
    if(!loaded()) {
        return false;
    }
    auto root = wire_root(data);
    auto size = root[&EnvelopeBlob::preamble_size];
    return text.size() >= size &&
           llvm::xxh3_64bits(text.take_front(size)) == root[&EnvelopeBlob::preamble_hash];
}

std::vector<feature::DocumentLink> TUIndex::links() const {
    if(!loaded()) {
        return {};
    }
    auto entries = wire_root(data)[&EnvelopeBlob::links];

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

llvm::ArrayRef<std::uint32_t> TUIndex::inactive_regions() const {
    if(!loaded()) {
        return {};
    }
    return to_array_ref(wire_root(data)[&EnvelopeBlob::inactive_regions]);
}

llvm::ArrayRef<std::uint8_t> TUIndex::open_conditionals() const {
    if(!loaded()) {
        return {};
    }
    return to_array_ref(wire_root(data)[&EnvelopeBlob::open_conditionals]);
}

}  // namespace clice::index
