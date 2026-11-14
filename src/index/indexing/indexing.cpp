module clice;

import :compile.compilation;
import :compile.directive;
import :index.format;
import :index.indexing;
import :index.indexing.builder;
import :index.indexing.include_tree;
import :index.indexing.layout;
import :index.indexing.tu_index;
import :index.shard;
import :index.shard.instantiations;
import :index.symbol;
import :semantic.comment;
import :semantic.decls;
import :semantic.identity;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.token_class;
import :support.logging;
import :support.text;
import :syntax.lexical_scan;
import :syntax.token;

namespace clice::index {

SymbolScope scope_of(const clang::NamedDecl* decl) {
    // A template parameter is named only inside its template, whatever
    // linkage Clang derives for it from the enclosing context.
    if(llvm::isa<clang::TemplateTypeParmDecl,
                 clang::NonTypeTemplateParmDecl,
                 clang::TemplateTemplateParmDecl>(decl)) {
        return SymbolScope::FunctionLocal;
    }
    // Module linkage reaches the module's other units, as external
    // linkage reaches every unit.
    auto linkage = decl->getFormalLinkage();
    if(linkage == clang::Linkage::Internal) {
        return SymbolScope::Internal;
    }
    if(linkage != clang::Linkage::None) {
        return SymbolScope::External;
    }
    // A name without linkage still reaches as far as its scope does: an
    // alias, an enumerator of an unnamed enum (every enumerator in C) or a
    // member of an unnamed class is named from every file including its
    // header. Only a function's own declarations and a prototype's
    // parameters stay in their file.
    if(decl->getParentFunctionOrMethod() || llvm::isa<clang::ParmVarDecl>(decl)) {
        return SymbolScope::FunctionLocal;
    }
    return decl->isInAnonymousNamespace() ? SymbolScope::Internal : SymbolScope::External;
}

namespace {

/// Occurrences cover disjoint ranges: a name inside another symbol's
/// written name — the class in `~Foo` — is that name's, and the symbol it
/// names keeps only its relation there.
void drop_nested(std::vector<Occurrence>& occurrences) {
    ranges::sort(occurrences, [](const Occurrence& lhs, const Occurrence& rhs) {
        return std::tuple(lhs.range.begin, rhs.range.end) <
               std::tuple(rhs.range.begin, lhs.range.end);
    });
    std::optional<LocalSourceRange> outer;
    std::erase_if(occurrences, [&](const Occurrence& occurrence) {
        auto range = occurrence.range;
        if(outer && range != *outer && range.end <= outer->end) {
            return true;
        }
        if(!outer || range.end > outer->end) {
            outer = range;
        }
        return false;
    });
}

/// Whether a member access names its member by a qualified name
/// (`object.Base::draw`), which calls it without virtual dispatch.
bool names_member_qualified(const SemanticNode& node) {
    if(const auto* member = node.get<clang::MemberExpr>()) {
        return member->hasQualifier();
    }
    if(const auto* member = node.get<clang::UnresolvedMemberExpr>()) {
        return bool(member->getQualifierLoc());
    }
    if(const auto* member = node.get<clang::CXXDependentScopeMemberExpr>()) {
        return bool(member->getQualifierLoc());
    }
    return false;
}

}  // namespace

Builder::Builder(CompilationUnit& unit, const TUIndexOptions& options) :
    unit(unit), focus_only(unit.kind() == CompilationKind::Content && !options.all_files),
    known_variants(options.known_variants), resolver(unit, {.only_instantiation = false}),
    readable(unit.context(), {}), known_instantiations(options.known_instantiations),
    standard_library(options.standard_library_instantiations),
    library_bodies(options.library_bodies) {}

FileIndex* Builder::file_index(clang::FileID fid) {
    if(instantiation_entries) {
        return &(*instantiation_entries)[fid];
    }
    if(focus_only && fid != unit.focus_file().file() && !module_files.contains(fid)) {
        return nullptr;
    }
    // LIMITATION: what the host's context declares is the host's to index:
    // with indexing off, or before the background index reaches a closed
    // host, a header compiled in its context finds no definition of what the
    // host's earlier includes declare (hover still shows it).
    if(unit.from_context(fid)) {
        return nullptr;
    }
    return &file_indices[fid];
}

index::SymbolHash Builder::ensure_symbol(const clang::NamedDecl* decl) {
    if(instantiation_entries) {
        decl = in_pattern(decl);
    }
    auto hash = unit.get<EntityTable>().entity(decl);
    auto [it, success] = symbols.try_emplace(hash);
    if(success) {
        // The entity's own declaration describes it: a reference can
        // reach it first through a template (a member body calling
        // `begin<T>()` above `begin`) or a using-declaration's shadow,
        // which share its hash (EntityTable::entity).
        const auto* described = decl;
        if(const auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(described)) {
            described = shadow->getTargetDecl();
        }
        if(const auto* tmpl = llvm::dyn_cast<clang::RedeclarableTemplateDecl>(described)) {
            described = tmpl->getTemplatedDecl();
        }
        it->second = describe(described);
        if(focus_only && decl->isFromASTFile() && decl->isInNamedModule() &&
           !llvm::isa<clang::NamespaceDecl>(decl)) {
            imported.push_back(decl);
        }
        if(it->second.scope != SymbolScope::External) {
            internal_decls.try_emplace(hash, described);
        }
    }
    return hash;
}

void Builder::add_occurrence(const clang::NamedDecl* decl, clang::SourceRange name) {
    auto [fid, range] = *unit.written_range(name);
    auto* index = file_index(fid);
    if(!index) {
        return;
    }

    index->occurrences.emplace_back(range, ensure_symbol(decls::normalize(decl)));
}

SymbolHash Builder::ensure_macro(SymbolHash hash,
                                 clang::SourceLocation name,
                                 const clang::MacroInfo* def) {
    auto [it, inserted] = symbols.try_emplace(hash);
    if(!inserted) {
        return hash;
    }
    auto& symbol = it->second;
    symbol.name = unit.token_spelling(name);
    symbol.kind = SymbolKind::Macro;
    symbol.scope = SymbolScope::External;
    // An include guard is no name to complete, nor is a macro its
    // header undefines.
    auto flags = def && !def->isUsedForHeaderGuard() && !undefined.contains(def)
                     ? SymbolFlags::Completable
                     : SymbolFlags::None;
    if(def && unit.in_library(def->getDefinitionLoc())) {
        flags |= SymbolFlags::SystemHeader;
    }
    symbol.flags = flags;
    symbol.token_class =
        def ? token_class(*def, unit) : TokenClass{.kind = SemanticTokenKind::Macro};
    return hash;
}

void Builder::add_macro(const clang::MacroInfo* def,
                        RelationKind kind,
                        clang::SourceLocation location) {
    // A name a macro body spells is the expansions' to resolve
    // (project_expansions). One pasted together (`CAT(BUF, SIZE)`) is
    // spelled by no text: a reference at the expansion producing it,
    // beside no occurrence, as no token there is the macro's.
    if(location.isMacroID()) {
        if(!unit.concatenated(location)) {
            return;
        }
        auto [fid, offset] = unit.produced_at(location);
        if(auto* index = file_index(fid)) {
            auto hash = ensure_macro(unit.get<EntityTable>().entity(def), location, def);
            index->relations[hash].push_back({
                .kind = kind,
                .range = {offset, offset + unit.token_length(location)},
                .target = 0,
            });
        }
        return;
    }

    auto [fid, range] = *unit.written_range(location);
    auto* index = file_index(fid);
    if(!index) {
        return;
    }

    auto hash = ensure_macro(unit.get<EntityTable>().entity(def), location, def);
    index->occurrences.emplace_back(range, hash);

    Relation relation{
        .kind = kind,
        .range = range,
        .target = 0,
    };

    // Definition relations carry the macro's full extent (name through
    // last body token), like declarations do — definition-text
    // consumers decode it out of target.
    if(kind.declares()) {
        auto [def_fid, def_range] = *unit.written_range(
            clang::SourceRange(def->getDefinitionLoc(), def->getDefinitionEndLoc()));
        if(def_fid == fid) {
            relation.set_extent(def_range);
        }
    }

    index->relations[hash].emplace_back(relation);
}

void Builder::add_unresolved_macro(clang::SourceLocation location) {
    auto [fid, range] = *unit.written_range(location);
    auto* index = file_index(fid);
    if(!index) {
        return;
    }
    auto hash = ensure_macro(EntityTable::unresolved_macro(unit.token_spelling(location)),
                             location,
                             nullptr);
    index->relations[hash].push_back(
        {.kind = RelationKind::Reference, .range = range, .target = 0});
}

void Builder::index_modules(const Semantics& semantics) {
    auto emit =
        [&](llvm::StringRef name, clang::FileID fid, LocalSourceRange range, RelationKind kind) {
            if(name.empty())
                return;
            auto* index = file_index(fid);
            if(!index)
                return;
            auto hash = EntityTable::module_entity(name);

            index->occurrences.emplace_back(range, hash);
            Relation relation{
                .kind = kind,
                .range = range,
                .target = 0,
            };
            // Decl/def consumers read the definition range out of
            // target; without it, module symbols would report their
            // definition as missing.
            if(kind.declares()) {
                relation.set_extent(range);
            }
            index->relations[hash].emplace_back(relation);

            auto& symbol = symbols[hash];
            if(symbol.name.empty()) {
                symbol.name = name.str();
                symbol.kind = SymbolKind::Module;
                symbol.scope = SymbolScope::External;
                symbol.token_class = {.kind = SemanticTokenKind::Module};
            }
        };

    // Import sites: Reference relations at the spelled module name. The
    // expansion range keeps macro-spelled names (`import MOD;`) anchored
    // at the import site instead of the macro definition.
    for(auto& [fid, directive]: unit.directives()) {
        for(auto& import: directive.imports) {
            if(import.name_locations.empty())
                continue;
            auto [loc_fid, range] = unit.produced_range(
                clang::SourceRange(import.name_locations.front(), import.name_locations.back()));
            llvm::StringRef name = import.full_name.empty() ? import.name : import.full_name;
            emit(name, loc_fid, range, RelationKind::Reference);
        }
    }

    // The module declaration of this unit: Definition in an interface
    // unit or a partition, Declaration in an implementation unit. The
    // declaration has no AST node or PP location; the semantics' lexical
    // scan located and cross-checked its written tokens. The occurrence
    // spans the written name, partition included.
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
        emit(module_name,
             unit.focus_file().file(),
             module.name_range(),
             unit.defines_module() ? RelationKind::Definition : RelationKind::Declaration);
        break;
    }
}

clang::SourceRange Builder::add_name(const NameOccurrence& occurrence) {
    // Macro-generated names anchor at written source, following
    // clang's file-location rule: argument tokens at their
    // spelling in the invocation, body and pasted tokens at the
    // invocation point. No written entry anchors inside a
    // `#define` body — those tokens have no meaning of their own,
    // each expansion assigns one: what a body names is a resolution of
    // its expansion (project_expansions).
    auto [name, spelled] =
        unit.seen_name({occurrence.location,
                        occurrence.name_end.isValid() ? occurrence.name_end : occurrence.location});
    auto location = name.getBegin();
    if(!instantiation_entries && occurrence.location.isMacroID()) {
        body_names.push_back({occurrence.decl, occurrence.kind, occurrence.location});
    }

    // An occurrence says "this range spells the name", so it
    // exists only where that holds. Names conjured by a macro
    // body or token paste still get the self-relation below —
    // reference lists and jump targets keep the invocation relation —
    // but the invocation token itself stays the macro's, not
    // theirs.
    if(spelled) {
        add_occurrence(occurrence.decl,
                       occurrence.owns_whole_name() ? name : clang::SourceRange(location));
    }
    if(occurrence.later_piece) {
        return name;
    }

    // Every occurrence is mirrored as a self-relation spanning
    // the whole name, so find-references on the occurring decl
    // finds this relation and cursor-site detection finds the
    // occurrence inside it.
    add_self_relation(occurrence.decl, occurrence.kind, name);
    return name;
}

void Builder::project_imported() {
    llvm::SmallVector<NameOccurrence> declarations;
    for(const auto* decl: imported) {
        for(const auto* redecl: decl->redecls()) {
            if(!redecl->isFromASTFile() || !redecl->isInNamedModule()) {
                continue;
            }
            for(auto& reference: resolve_references(SemanticNode(redecl))) {
                if(reference.location.isValid() && reference.kind.declares()) {
                    declarations.push_back(
                        {reference.decl, reference.kind, reference.location, reference.name_end});
                    module_files.insert(unit.seen_at(reference.location).file);
                }
            }
        }
    }
    for(auto& declaration: declarations) {
        add_name(declaration);
    }
}

void Builder::see_from(const Semantics& semantics, std::uint32_t i) {
    const auto* decl = semantics.node(i).node.get<clang::Decl>();
    if(!decl) {
        return;
    }
    auto location = decl->getLocation();
    if(unit.in_library(location) && !unit.focus_file().contains(unit.file_id(location))) {
        place_resolver = nullptr;
        return;
    }
    place_resolver = &resolver;
    resolver.see_from(location);
}

void Builder::project_node(const Semantics& semantics, std::uint32_t i) {
    enclosing_memo.resize(std::min<std::size_t>(enclosing_memo.size(), i));
    top_level_memo.resize(std::min<std::size_t>(top_level_memo.size(), i));
    // What an instantiation resolves are the calls and member accesses
    // its body's expressions make and the types its placeholders
    // deduce; it declares nothing its template does not, and the types
    // it substitutes are no references.
    const auto* stmt = semantics.node(i).node.get<clang::Stmt>();
    expression_begin = stmt ? stmt->getBeginLoc() : clang::SourceLocation();
    see_from(semantics, i);
    // With no resolver to narrow it, an overload set a library's template
    // names is what its definition's lookup found, which follows what the
    // unit declared before the header: each unit would store its own
    // variant of the header.
    if(!place_resolver && stmt && llvm::isa<clang::OverloadExpr>(stmt)) {
        return;
    }
    if(instantiation_entries && !stmt) {
        project_placeholder(semantics, i);
        if(const auto* value = semantics.node(i).node.get<clang::ValueDecl>()) {
            add_deduced_type(value);
        }
        return;
    }
    for(auto& occurrence: resolve_occurrences(semantics, i, place_resolver)) {
        auto name = add_name(occurrence);
        if(instantiation_entries || occurrence.later_piece) {
            continue;
        }

        if(occurrence.kind.is_one_of(RelationKind::Reference)) {
            // What an instantiation passes differs from one
            // instantiation to the next: the written code only.
            if(!semantics.node(i).flags.in_instantiation && is_written(semantics, i)) {
                add_self_relation(occurrence.decl, RelationKind::Write, name);
            }
            add_specialized_type(occurrence.decl, name);
        }
        if(occurrence.kind.is_one_of(RelationKind::Reference,
                                     RelationKind::Candidate,
                                     RelationKind::Guess) &&
           names_member_qualified(semantics.node(i).node)) {
            if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(occurrence.decl);
               method && method->isVirtual()) {
                add_self_relation(occurrence.decl, RelationKind::Direct, name);
            }
        }

        if(!occurrence.kind.is_one_of(RelationKind::Declaration,
                                      RelationKind::Definition,
                                      RelationKind::Candidate,
                                      RelationKind::Guess)) {
            add_pasted_relation(occurrence.decl,
                                enclosing_top_level(semantics, i),
                                name.getBegin());
        }
    }
    // The focus file's comments alone: scanning every header's would
    // cost each unit's index a lex of all it includes, and a header the
    // editor opens has a compile of its own.
    if(const auto* decl = semantics.node(i).node.get<clang::Decl>();
       decl && !semantics.node(i).flags.in_instantiation &&
       unit.file_id(decl->getLocation()) == unit.focus_file().file()) {
        // FIXME: the mention is a Reference, so document highlight shows a
        // comment's `@param value` as a read; showing it as text (still in
        // references and rename) needs a relation kind of its own.
        for(auto& documented: documented_parameters(unit, decl)) {
            add_name({.decl = documented.parameter,
                      .kind = RelationKind::Reference,
                      .location = documented.location});
        }
    }

    project_placeholder(semantics, i);
    project_relations(semantics, i);
}

void Builder::project_macros() {
    for(auto& [fid, directive]: unit.directives()) {
        for(auto& macro: directive.macros) {
            if(macro.kind == MacroRef::Kind::Undef &&
               unit.file_id(macro.macro->getDefinitionLoc()) == fid) {
                undefined.insert(macro.macro);
            }
        }
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
        for(auto location: directive.unresolved_macros) {
            add_unresolved_macro(location);
        }
    }
}

std::vector<std::byte> Builder::build() {
    Timer written_timer;
    auto walked = walked_instantiations();
    // The focus-only shape is the one features share, cached on the
    // unit; the whole-TU shape is walked node by node, projected as it
    // is reached. A focus compile also walks the declarations of other
    // files that expand its macros; the instantiations are a walk of
    // their own in either shape.
    std::optional<Semantics> whole;
    // LIMITATION: the whole-TU shape walks every header the unit includes in
    // full, and only on writing finds most of them equal to the stored ones
    // (known_variants): a full index takes up to several times clangd's,
    // which indexes each header once per project.
    if(!focus_only) {
        whole.emplace(
            Semantics::build(unit,
                             {.all_files = true,
                              .visit = [&](const Semantics& path,
                                           std::uint32_t index) { project_node(path, index); },
                              .library_bodies = library_bodies}));
    } else {
        const auto& focus = unit.get<Semantics>();
        for(auto [index, entry]: views::enumerate(focus.node_entries())) {
            // Macros are projected from the directives below; includes
            // and imports have their own pipelines. What an explicit
            // instantiation's members hold is the instantiations' walk's,
            // as in the whole-TU shape, which meets them there alone.
            if(entry.node.is_ast() && !entry.flags.in_instantiation) {
                project_node(focus, static_cast<std::uint32_t>(index));
            }
        }
        if(auto expanding = expanding_decls(); !expanding.empty()) {
            Semantics::build(
                unit,
                {.all_files = true,
                 .visit = [&](const Semantics& path,
                              std::uint32_t index) { collect_body_names(path, index); },
                 .written = expanding});
        }
    }
    project_macros();
    index_modules(whole ? *whole : unit.get<Semantics>());
    project_imported();
    auto written_ms = written_timer.precise_ms();

    Timer instantiations_timer;
    project_instantiations(walked);
    for(const auto* decl: unit.instantiations()) {
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
            project_bases(record);
        }
    }
    // LIMITATION: what a preamble's macros expand to is the background
    // index's to resolve: with indexing off, or before the background index
    // reaches the file, an open file misses the names its headers' macros
    // expand to. The preamble's own index carries what it instantiates alone,
    // which the compile loading its PCH never instantiates again.
    if(unit.kind() != CompilationKind::Preamble) {
        project_expansions();
    }
    auto instantiations_ms = instantiations_timer.precise_ms();

    Timer finish_timer;
    // Build the include graph from what the index actually recorded:
    // every fid keying `file_indices` gets its include chain resolved
    // through the SourceManager, so the lookups below cannot miss.
    llvm::SmallVector<clang::FileID, 16> indexed_fids;
    indexed_fids.reserve(file_indices.size() + resolved.size());
    for(auto& [fid, index]: file_indices) {
        indexed_fids.push_back(fid);
    }
    for(auto fid: views::transform(resolved, [](auto& entry) -> auto& { return entry.first; })) {
        if(!file_indices.contains(fid)) {
            indexed_fids.push_back(fid);
        }
    }
    // The unit writes no entries for what it reads through the
    // precompiled header or a module, yet a name below external scope
    // declared there (a header `static`, a header function's class the
    // unit reaches through `auto`) is named in the declaring files as
    // much as in the unit's own. A header module's files (-fmodules) are
    // no unit's to index.
    llvm::SmallVector<std::pair<SymbolHash, clang::FileID>> declaring_files;
    llvm::DenseSet<clang::FileID> queued(indexed_fids.begin(), indexed_fids.end());
    for(auto [hash, decl]: internal_decls) {
        for(const auto* redecl: decl->redecls()) {
            auto* module = redecl->getOwningModule();
            auto fid = unit.seen_at(redecl->getLocation()).file;
            if(redecl->isFromASTFile() && !(module && module->isHeaderLikeModule()) &&
               fid.isValid() && !unit.from_context(fid)) {
                declaring_files.emplace_back(hash, fid);
                if(queued.insert(fid).second) {
                    indexed_fids.push_back(fid);
                }
            }
        }
    }
    tree = IncludeTree::from(unit, indexed_fids);
    for(auto [hash, fid]: declaring_files) {
        if(tree.node_of(fid) != no_node) {
            symbols.find(hash)->second.reference_files.add(tree.path_id(fid));
        }
    }

    // Entries are keyed by FileID while they accumulate, by path from here
    // on. A header entered several times (its FileIDs differ, its path
    // id does not) contributes the union of its FileIDs' entries:
    // write_shard canonicalizes — sorts and deduplicates — so
    // concatenation is union. A file with no include edge is a
    // synthetic buffer (predefines, <command line>): it has no real
    // path to attribute entries to, and path_id() would misfile them
    // under the source file. Real files forced in via -include are not
    // affected — clang records their include edge in the predefines
    // buffer, which is a valid location. The focus file legitimately
    // has no edge.
    llvm::DenseMap<std::uint32_t, FileIndex> by_path;
    llvm::DenseMap<std::uint32_t, clang::FileID> path_fids;
    for(auto& [fid, index]: file_indices) {
        if(fid != unit.focus_file().file() && tree.node_of(fid) == no_node) {
            continue;
        }
        auto path_id = tree.path_id(fid);
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

    for(auto& file_index:
        views::transform(by_path, [](auto& entry) -> auto& { return entry.second; })) {
        drop_nested(file_index.occurrences);
    }

    // Instantiations by path alike: what each FileID of a header resolves
    // is one set of resolutions and keys.
    llvm::DenseMap<std::uint32_t, Resolved> resolved_by_path;
    for(auto& [fid, file]: resolved) {
        if(fid != unit.focus_file().file() && tree.node_of(fid) == no_node) {
            continue;
        }
        auto path_id = tree.path_id(fid);
        path_fids.try_emplace(path_id, fid);
        auto& into = resolved_by_path[path_id];
        auto offset = static_cast<std::uint32_t>(into.walked.resolutions.size());
        for(auto& resolution: file.walked.resolutions) {
            into.walked.resolutions.push_back(std::move(resolution));
        }
        for(auto key: file.walked.keys) {
            if(key.resolution) {
                *key.resolution += offset;
            }
            into.walked.keys.push_back(key);
        }
        into.skipped.append_range(file.skipped);
    }
    // An external symbol lists the files whose instantiations name it on
    // their own; any other reaches them as its reference files.
    auto named_in = [&](SymbolHash hash, std::uint32_t path_id) {
        auto it = symbols.find(hash);
        if(it == symbols.end()) {
            return;
        }
        if(it->second.scope == SymbolScope::External) {
            it->second.instantiation_files.add(path_id);
        } else {
            it->second.reference_files.add(path_id);
        }
    };
    for(auto& [path_id, file]: resolved_by_path) {
        for(auto& resolution: file.walked.resolutions) {
            for(auto& record: resolution) {
                named_in(record.target, path_id);
                if(has_from(record.kind)) {
                    named_in(record.from, path_id);
                }
            }
        }
    }

    for(auto& [path_id, file_index]: by_path) {
        for(auto& [symbol_id, relations]: file_index.relations) {
            auto& symbol = symbols[symbol_id];
            symbol.reference_files.add(path_id);
            for(auto& relation: relations) {
                if(relation.kind == RelationKind::Definition) {
                    symbol.definition_files.add(path_id);
                    symbol.declaration_files.add(path_id);
                } else if(relation.kind == RelationKind::Declaration) {
                    symbol.declaration_files.add(path_id);
                } else if(RelationKind(relation.kind).between_symbols()) {
                    // A query anchors a relation's target at the file
                    // holding the relation; an internal one reaches its
                    // own entries from there only through this file.
                    if(auto it = symbols.find(relation.target);
                       it != symbols.end() && it->second.scope != SymbolScope::External) {
                        it->second.reference_files.add(path_id);
                    }
                }
            }
        }
    }
    auto finish_ms = finish_timer.precise_ms();

    Timer encode_timer;
    auto resolve = [&](SymbolHash hash) -> std::optional<SymbolView> {
        auto it = symbols.find(hash);
        if(it == symbols.end()) {
            return std::nullopt;
        }
        return it->second.view();
    };

    llvm::SmallVector<std::uint32_t> path_ids;
    path_ids.reserve(by_path.size());
    for(auto path_id: views::transform(by_path, [](auto& entry) -> auto& { return entry.first; })) {
        path_ids.push_back(path_id);
    }
    ranges::sort(path_ids);
    std::vector<FileSection> sections;
    llvm::BitVector known_paths(tree.paths.size());
    for(auto path_id: path_ids) {
        auto& file_index = by_path[path_id];
        if(file_index.empty()) {
            continue;
        }
        auto bytes = write_shard(file_index, resolve, unit.file_content(path_fids[path_id]));
        auto hash = llvm::xxh3_64bits(as_chars(bytes));
        if(ranges::binary_search(known_variants, known_at(tree.paths[path_id], hash))) {
            known_paths.set(path_id);
            sections.push_back({path_id, hash, {}});
            continue;
        }
        sections.push_back({path_id, hash, std::move(bytes)});
    }
    std::vector<InstantiationSection> instantiation_sections;
    auto resolved_paths = ranges::to<llvm::SmallVector<std::uint32_t>>(
        views::transform(resolved_by_path, [](auto& entry) { return entry.first; }));
    ranges::sort(resolved_paths);
    for(auto path_id: resolved_paths) {
        auto& file = resolved_by_path[path_id];
        InstantiationSection section{.path_id = path_id};
        if(!file.walked.empty()) {
            auto content = unit.file_content(path_fids[path_id]);
            section.blob = write_instantiations(file.walked,
                                                llvm::xxh3_64bits(content),
                                                static_cast<std::uint32_t>(content.size()));
        }
        ranges::sort(file.skipped);
        file.skipped.erase(ranges::unique(file.skipped).begin(), file.skipped.end());
        section.skipped = std::move(file.skipped);
        instantiation_sections.push_back(std::move(section));
    }
    auto encode_ms = encode_timer.precise_ms();

    TUIndexBlob blob;
    blob.format_version = index_format_version;
    blob.built_at = unit.started_at().count();
    blob.paths = std::move(tree.paths);
    blob.content_hashes = std::move(tree.content_hashes);
    blob.nodes = std::move(tree.nodes);
    for(auto& link: tree.links) {
        blob.link_files.push_back(link.file);
        blob.link_lines.push_back(link.line);
        blob.link_columns.push_back(link.column);
        blob.link_targets.push_back(std::move(link.target));
    }
    blob.absent = unit.absent();
    llvm::SmallVector<SymbolHash> table;
    for(auto& [hash, symbol]: symbols) {
        // A function's own name stays out but where another file names
        // it: the merge gives a symbol several files hold a record of them.
        if(symbol.scope == SymbolScope::FunctionLocal && symbol.reference_files.cardinality() < 2) {
            continue;
        }
        if(symbol.scope == SymbolScope::External && !symbol.reference_files.isEmpty() &&
           symbol.instantiation_files.isEmpty() &&
           std::all_of(symbol.reference_files.begin(),
                       symbol.reference_files.end(),
                       [&](std::uint32_t path_id) { return known_paths.test(path_id); })) {
            continue;
        }
        table.push_back(hash);
    }
    ranges::sort(table);
    for(auto hash: table) {
        auto& symbol = symbols.find(hash)->second;
        blob.symbol_hashes.push_back(hash);
        blob.symbol_names += symbol.name;
        blob.symbol_name_ends.push_back(static_cast<std::uint32_t>(blob.symbol_names.size()));
        blob.symbol_args += symbol.args;
        blob.symbol_args_ends.push_back(static_cast<std::uint32_t>(blob.symbol_args.size()));
        blob.symbol_signatures += symbol.signature;
        blob.symbol_signature_ends.push_back(
            static_cast<std::uint32_t>(blob.symbol_signatures.size()));
        blob.symbol_parents.push_back(symbol.parent);
        blob.symbol_kinds.push_back(symbol.kind.value());
        blob.symbol_scopes.push_back(static_cast<std::uint8_t>(symbol.scope));
        auto view = symbol.view();
        blob.symbol_flags.push_back(static_cast<std::uint16_t>(view.flags));
        blob.symbol_token_kinds.push_back(static_cast<std::uint8_t>(symbol.token_class.kind));
        blob.symbol_token_modifiers.push_back(symbol.token_class.modifiers);
        blob.symbol_files.push_back(view.file);
        for(auto file: symbol.reference_files) {
            blob.symbol_references.push_back(file);
        }
        blob.symbol_reference_ends.push_back(
            static_cast<std::uint32_t>(blob.symbol_references.size()));
        for(auto file: symbol.instantiation_files) {
            blob.symbol_instantiations.push_back(file);
        }
        blob.symbol_instantiation_ends.push_back(
            static_cast<std::uint32_t>(blob.symbol_instantiations.size()));
    }
    blob.sections = std::move(sections);
    blob.instantiations = std::move(instantiation_sections);
    Timer pack_timer;
    auto bytes = encode(blob);

    LOG_PERF("index_detail",
             "op=build scope={} written_ms={:.2f} "
             "instantiations_ms={:.2f} instantiations={} walked={} finish_ms={:.2f} "
             "encode_ms={:.2f} pack_ms={:.2f}",
             focus_only ? "focus" : "full",
             written_ms,
             instantiations_ms,
             unit.instantiations().size(),
             walked.size(),
             finish_ms,
             encode_ms,
             pack_timer.precise_ms());
    return bytes;
}

std::vector<std::byte> build_tu_index(CompilationUnit& unit, const TUIndexOptions& options) {
    Builder indexer(unit, options);
    return indexer.build();
}

}  // namespace clice::index
