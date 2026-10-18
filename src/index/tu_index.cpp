module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :compile.compilation;
import :index.instantiation_shard;
import :index.serialization;
import :index.shard;
import :index.tu_index;
import :semantic.decls;
import :semantic.display;
import :semantic.identity;
import :semantic.resolver;
import :semantic.semantics;
import :semantic.token_class;
import :semantic.types;
import :support.logging;
import :support.timer;

namespace clice::index {

SymbolScope classify_scope(const clang::NamedDecl* decl) {
    // A template parameter is named only inside its template, whatever
    // linkage Clang derives for it from the enclosing context.
    if(llvm::isa<clang::TemplateTypeParmDecl,
                 clang::NonTypeTemplateParmDecl,
                 clang::TemplateTemplateParmDecl>(decl)) {
        return SymbolScope::FileLocal;
    }
    // Module linkage reaches the module's other units, as external
    // linkage reaches every unit.
    auto linkage = decl->getFormalLinkage();
    if(linkage == clang::Linkage::Internal) {
        return SymbolScope::TULocal;
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
        return SymbolScope::FileLocal;
    }
    return decl->isInAnonymousNamespace() ? SymbolScope::TULocal : SymbolScope::External;
}

namespace {

/// Whether an anonymous namespace sits between a declaration and the scope
/// its qualified name starts from, past the scopes such a name skips.
bool in_anonymous_namespace(const clang::NamedDecl* decl) {
    if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
        decl = shadow->getTargetDecl();
    }
    for(const auto* context = decl->getDeclContext(); !context->isTranslationUnit();
        context = context->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            if(ns->isAnonymousNamespace()) {
                return true;
            }
            if(!ns->isInline()) {
                return false;
            }
        } else if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(context);
                  record && record->isAnonymousStructOrUnion()) {
            continue;
        } else if(const auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(context);
                  enumeration && !enumeration->isScoped() && !enumeration->getIdentifier()) {
            continue;
        } else if(llvm::isa<clang::NamedDecl>(context)) {
            return false;
        }
    }
    return false;
}

/// One file's index in the TU index blob: a self-contained single-variant shard
/// blob (index/shard.h). `hash` is xxh3 of `blob` — the variant's
/// identity — so the master can skip blobs it already stores without
/// touching their bytes; the blob is left empty for a variant the master
/// said it stores.
struct FileSection {
    std::uint32_t path_id = 0;

    std::uint64_t hash = 0;

    std::vector<std::uint8_t> blob;
};

/// One file's instantiations in the TU index: the instantiation blob of what
/// the run resolved there, empty when it resolved nothing, and the known keys
/// whose definition the file holds that the run skipped, ascending.
struct InstantiationSection {
    std::uint32_t path_id = 0;

    std::vector<std::uint8_t> blob;

    std::vector<std::uint64_t> skipped;
};

/// The TU index's serialized layout. Only the builder below ever materializes
/// it; every consumer reads the bytes through the TUIndex reader.
struct TUIndexBlob {
    /// Serialized schema version (index_format_version), gated by
    /// TUIndex::from_bytes.
    /// A worker respawned after the binary on disk changed can be one
    /// build ahead of the server, and a layout change need not be
    /// structurally detectable.
    std::uint32_t format_version = 0;

    /// Milliseconds since epoch, sampled before the build started.
    std::int64_t built_at = 0;

    /// The include tree (IncludeTree's persisted vectors): the path table,
    /// the consumed-content hash per path, and every include edge of the
    /// parse.
    std::vector<std::string> paths;
    std::vector<std::uint64_t> path_hashes;
    std::vector<IncludeNode> nodes;

    /// The symbols the entries name, sorted by hash, except the file-local
    /// ones (a function's locals, a template's parameters) — only their
    /// own file can mention one, and that file's section names it — and
    /// the external symbols only sections left empty name. Names, arguments and signatures are
    /// back to back with one end offset per entry; so are the reference files, as path ids.
    std::vector<std::uint64_t> sym_hashes;
    std::string sym_names;
    std::vector<std::uint32_t> sym_name_ends;
    std::string sym_args;
    std::vector<std::uint32_t> sym_args_ends;
    std::string sym_signatures;
    std::vector<std::uint32_t> sym_signature_ends;
    std::vector<std::uint64_t> sym_parents;
    std::vector<std::uint8_t> sym_kinds;
    std::vector<std::uint8_t> sym_scopes;
    std::vector<std::uint16_t> sym_flags;
    std::vector<std::uint8_t> sym_token_kinds;
    std::vector<std::uint32_t> sym_token_modifiers;
    /// Path ids, `no_file` for a symbol without a declaring relation.
    std::vector<std::uint32_t> sym_files;
    std::vector<std::uint32_t> sym_reference_ends;
    std::vector<std::uint32_t> sym_references;

    /// Per symbol, the files whose instantiation section names it, back to
    /// back as path ids.
    std::vector<std::uint32_t> sym_instantiation_ends;
    std::vector<std::uint32_t> sym_instantiations;

    /// One entry per file with an index, ascending by path id.
    std::vector<FileSection> sections;

    /// One entry per file the run's instantiations or macro expansions
    /// resolve something in or skipped a known key of, ascending by path id.
    std::vector<InstantiationSection> instantiations;

    /// Preamble ride-alongs, empty on ordinary TU indexes: identity of the
    /// exact preamble text the PCH was built from (matches_prefix), and
    /// the PCH-derived feature state spliced into focus-file results. The
    /// refs borrow the builder's inputs — encode-only, like the section
    /// blobs are for readers.
    std::uint64_t preamble_hash = 0;
    std::uint32_t preamble_size = 0;
    llvm::ArrayRef<DocumentLink> links;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    llvm::ArrayRef<std::uint8_t> open_conditionals;

    /// The places the parse's failed lookups looked that held no file
    /// (CompilationUnit::absent).
    std::vector<std::string> absent;

    /// Preamble ride-along: the diagnostics the preamble's build raised,
    /// as published (a JSON array of LSP diagnostics) — a compile that
    /// consumes the PCH never raises them again.
    std::string diagnostics;
};

/// What build_preamble_index adds on top of an ordinary build.
struct PreambleExtras {
    std::uint64_t hash = 0;
    std::uint32_t size = 0;
    llvm::ArrayRef<DocumentLink> links;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    llvm::ArrayRef<std::uint8_t> open_conditionals;
    llvm::StringRef diagnostics;
};

NameForm name_form_of(clang::DeclarationName name) {
    switch(name.getNameKind()) {
        case clang::DeclarationName::Identifier: return NameForm::Identifier;
        case clang::DeclarationName::CXXConstructorName: return NameForm::Constructor;
        case clang::DeclarationName::CXXDestructorName: return NameForm::Destructor;
        case clang::DeclarationName::CXXConversionFunctionName: return NameForm::Conversion;
        case clang::DeclarationName::CXXOperatorName: return NameForm::Operator;
        case clang::DeclarationName::CXXLiteralOperatorName: return NameForm::Literal;
        case clang::DeclarationName::CXXDeductionGuideName:
        case clang::DeclarationName::CXXUsingDirective:
        case clang::DeclarationName::ObjCZeroArgSelector:
        case clang::DeclarationName::ObjCOneArgSelector:
        case clang::DeclarationName::ObjCMultiArgSelector: return NameForm::Other;
    }
    std::unreachable();
}

/// Instantiations never reach the symbol table (decls::normalize folds
/// them into their pattern), so a specialization node here was written.
bool is_specialization(const clang::NamedDecl* decl) {
    if(llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
           decl)) {
        return true;
    }
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization;
    }
    return false;
}

/// The type of a value; of a function, the type it returns.
clang::QualType value_type(const clang::ValueDecl* decl) {
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getReturnType();
    }
    return decl->getType();
}

/// The declaration go-to-type-definition reaches from a value or a
/// function: the one its value_type names once pointers, references and
/// arrays are stripped, and inside a template once the resolver resolved it.
const clang::NamedDecl* type_definition_of(const clang::ValueDecl* decl,
                                           types::TemplateResolver& resolver) {
    return types::decl_of(types::unwrap(resolver.resolve(value_type(decl))), &resolver);
}

/// Occurrences cover disjoint ranges: a name inside another symbol's
/// written name — the class in `~Foo` — is that name's, and the symbol it
/// names keeps only its relation there.
void drop_nested(std::vector<Occurrence>& occurrences) {
    std::ranges::sort(occurrences, [](const Occurrence& lhs, const Occurrence& rhs) {
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

/// The name a call is written through, where call hierarchy points at it:
/// `f` of `ns::f(x)`, `operator==` of `a.operator==(b)`, the operator token
/// of `a == b`, the pointer of `(*fp)(x)` or `(obj.*pmf)(x)`.
clang::SourceRange callee_name(const clang::CallExpr* call, const clang::SourceManager& SM) {
    // `12_km` names its operator with a suffix no token of its own spells.
    if(auto* UDL = llvm::dyn_cast<clang::UserDefinedLiteral>(call)) {
        return UDL->getBeginLoc();
    }

    const clang::Expr* callee = call->getCallee()->IgnoreParenImpCasts();
    if(auto* UO = llvm::dyn_cast<clang::UnaryOperator>(callee);
       UO && UO->getOpcode() == clang::UO_Deref) {
        callee = UO->getSubExpr()->IgnoreParenImpCasts();
    } else if(auto* BO = llvm::dyn_cast<clang::BinaryOperator>(callee); BO && BO->isPtrMemOp()) {
        callee = BO->getRHS()->IgnoreParenImpCasts();
    }

    if(auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(callee)) {
        return written_name(DRE->getNameInfo(), SM);
    }
    if(auto* ME = llvm::dyn_cast<clang::MemberExpr>(callee)) {
        return written_name(ME->getMemberNameInfo(), SM);
    }
    if(auto* OE = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        return written_name(OE->getNameInfo(), SM);
    }
    if(auto* DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        return written_name(DSDRE->getNameInfo(), SM);
    }
    if(auto* DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        return written_name(DSME->getMemberNameInfo(), SM);
    }
    return callee->getExprLoc();
}

/// The template an explicit or partial specialization specializes: a
/// class, variable or function template's pattern, or the member of a
/// class template a member specialization replaces. Null for every other
/// declaration, implicit and explicit instantiations included.
const clang::NamedDecl* specialized_template(const clang::NamedDecl* decl) {
    if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        if(llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(CTSD) ||
           CTSD->getSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return CTSD->getSpecializedTemplate()->getTemplatedDecl();
        }
        return nullptr;
    }
    if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
        if(llvm::isa<clang::VarTemplatePartialSpecializationDecl>(VTSD) ||
           VTSD->getSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return VTSD->getSpecializedTemplate()->getTemplatedDecl();
        }
        return nullptr;
    }
    if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if(FD->getTemplateSpecializationKind() != clang::TSK_ExplicitSpecialization) {
            return nullptr;
        }
        if(auto* primary = FD->getPrimaryTemplate()) {
            return primary->getTemplatedDecl();
        }
        return FD->getInstantiatedFromMemberFunction();
    }
    if(auto* CRD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        if(CRD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return CRD->getInstantiatedFromMemberClass();
        }
        return nullptr;
    }
    if(auto* VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if(VD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return VD->getInstantiatedFromStaticDataMember();
        }
    }
    return nullptr;
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

/// clangd's rule for what unqualified completion may offer from an index:
/// namespace-scope declarations, and the enumerators of unscoped enums at
/// namespace or class scope. Members are Sema's after `.`, and a
/// specialization shares its template's name.
bool is_completable(const clang::NamedDecl* decl) {
    if(is_specialization(decl)) {
        return false;
    }
    auto namespace_scope = [](const clang::DeclContext* context) {
        return context->isTranslationUnit() || context->isNamespace() ||
               llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context);
    };
    const clang::DeclContext* context = decl->getDeclContext();
    if(namespace_scope(context)) {
        return true;
    }
    if(auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(context)) {
        const clang::DeclContext* outer = enumeration->getDeclContext();
        return !enumeration->isScoped() &&
               (namespace_scope(outer) || llvm::isa<clang::CXXRecordDecl>(outer));
    }
    return false;
}

/// Projects the unit's semantic map into TUIndex entries: occurrences and
/// relations from the resolve facts, macros from the preprocessor directives.
class Projector {
public:
    Projector(CompilationUnit& unit, const TUIndexOptions& options) :
        unit(unit), focus_only(unit.kind() == CompilationKind::Content && !options.all_files),
        known_variants(options.known_variants),
        resolver(unit.context(), {.only_instantiation = false}), readable(unit.context(), {}),
        known_instantiations(options.known_instantiations),
        standard_library(options.standard_library_instantiations) {}

    /// The only gate through which entries enter `file_indices`. With
    /// focus_only, the index covers just the focus file and the
    /// declarations project_imported adds last — yet AST nodes reachable
    /// from its top-level decls can carry locations in other files:
    /// inherited default arguments and base specifiers of classes defined
    /// in a preamble header end up there. Such entries belong to the
    /// preamble's own index, so they are dropped here. So are the entries
    /// of a header's borrowed includer context: its host indexes them.
    FileIndex* file_index(clang::FileID fid) {
        if(space) {
            return &(*space)[fid];
        }
        if((focus_only && fid != unit.focus_file().file() && !module_files.contains(fid)) ||
           unit.from_context(fid)) {
            return nullptr;
        }
        return &file_indices[fid];
    }

    /// Symbol-table registration shared by every occurrence and relation
    /// writer: name lookups need the entry even when a symbol has only
    /// relations.
    index::SymbolHash ensure_symbol(const clang::NamedDecl* decl) {
        if(space) {
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
                auto& SM = unit.context().getSourceManager();
                internal_files.try_emplace(hash,
                                           SM.getFileID(SM.getFileLoc(described->getLocation())));
                if(!focus_only && it->second.scope == SymbolScope::TULocal &&
                   described->isFromASTFile()) {
                    precompiled_internal.emplace_back(hash, described);
                }
            }
        }
        return hash;
    }

    /// What the symbol table records about a declaration besides its
    /// index entries. The facts the entries decide — HasDefinition and the
    /// canonical file — are filled in once every entry is projected.
    Symbol describe(const clang::NamedDecl* decl) {
        Symbol symbol;
        auto flags = SymbolFlags::None;
        auto name = display::symbol_name(decl);
        symbol.name = std::move(name.text);
        if(name.unnamed) {
            flags |= SymbolFlags::Unnamed;
        }
        symbol.args = display::template_args(*decl);
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            symbol.signature = display::call_signature(*function,
                                                       {
                                                           .suppress_unwritten_scope = true,
                                                           .polish_for_declaration = true,
                                                           .clean_uglified = true,
                                                           .readable = &readable,
                                                           .scope = function->getDeclContext(),
                                                       });
        }
        symbol.parent = unit.get<EntityTable>().parent(decl);
        symbol.kind = SymbolKind::from(decl);
        symbol.scope = classify_scope(decl);
        if(decls::is_templated(decl)) {
            flags |= SymbolFlags::Template;
        }
        if(is_specialization(decl)) {
            flags |= SymbolFlags::Specialization;
        }
        // Attributes accumulate along the redeclaration chain; the last
        // declaration carries them all.
        if(decl->getMostRecentDecl()->getAvailability() == clang::AR_Deprecated) {
            flags |= SymbolFlags::Deprecated;
        }
        if(auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl); ns && ns->isInline()) {
            flags |= SymbolFlags::InlineNamespace;
        }
        if(auto* record = llvm::dyn_cast<clang::RecordDecl>(decl);
           record && record->isAnonymousStructOrUnion()) {
            flags |= SymbolFlags::AnonymousScope;
        }
        if(auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(decl);
           enumeration && !enumeration->isScoped() && !enumeration->getIdentifier()) {
            flags |= SymbolFlags::AnonymousScope;
        }
        if(in_anonymous_namespace(decl)) {
            flags |= SymbolFlags::AnonymousNamespace;
        }
        if(decl->isTemplateParameter()) {
            flags |= SymbolFlags::TemplateParameter;
        }
        // `FWD(Expr)` forward-declares a class defined by hand.
        if(llvm::all_of(decl->redecls(), [](const clang::Decl* redecl) {
               return redecl->getLocation().isMacroID();
           })) {
            flags |= SymbolFlags::SpelledInMacro;
        }
        if(unit.context().getSourceManager().isInSystemHeader(decl->getLocation())) {
            flags |= SymbolFlags::SystemHeader;
        }
        if(is_completable(decl)) {
            flags |= SymbolFlags::Completable;
        }
        if(decls::is_exported(decl)) {
            flags |= SymbolFlags::Exported;
        }
        symbol.flags = with_form(flags, name_form_of(decl->getDeclName()));
        symbol.token_class = token_class(decl);
        return symbol;
    }

    void add_occurrence(const clang::NamedDecl* decl, clang::SourceRange name) {
        auto [fid, range] = *unit.written_range(name);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        index->occurrences.emplace_back(range, ensure_symbol(decls::normalize(decl)));
    }

    void add_macro(const clang::MacroInfo* def, RelationKind kind, clang::SourceLocation location) {
        // FIXME: Figure out when location is MacroID.
        if(location.isMacroID()) {
            return;
        }

        auto [fid, range] = *unit.written_range(location);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        auto hash = unit.get<EntityTable>().entity(def);
        // Macros get a symbol-table entry like declarations do; without it
        // build() would default-construct a nameless entry when recording
        // reference files, and every name lookup for the macro would come
        // back empty.
        auto [it, success] = symbols.try_emplace(hash);
        if(success) {
            auto& symbol = it->second;
            symbol.name = unit.token_spelling(location);
            symbol.kind = SymbolKind::Macro;
            symbol.scope = SymbolScope::External;
            symbol.flags = SymbolFlags::Completable;
            symbol.token_class = def ? token_class(*def, unit.context().getSourceManager())
                                     : TokenClass{.kind = SemanticTokenKind::Macro};
        }
        index->occurrences.emplace_back(range, hash);

        Relation relation{
            .kind = kind,
            .range = range,
            .target_symbol = 0,
        };

        // Definition relations carry the macro's full extent (name through
        // last body token), like declarations do — definition-text
        // consumers decode it out of target_symbol.
        if(kind.isDeclOrDef() && def) {
            auto [def_fid, def_range] = *unit.written_range(
                clang::SourceRange(def->getDefinitionLoc(), def->getDefinitionEndLoc()));
            if(def_fid == fid) {
                relation.set_definition_range(def_range);
            }
        }

        index->relations[hash].emplace_back(relation);
    }

    /// A Definition/Declaration/Reference relation mirroring an occurrence:
    /// it spans the whole written name the occurrence lies in, and decl/def
    /// relations also carry the declaration's full extent.
    void add_self_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           clang::SourceRange name) {
        auto [fid, range] = *unit.written_range(name);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        Relation relation{.kind = kind, .range = range, .target_symbol = 0};

        if(kind.isDeclOrDef()) {
            // FIXME: why definition or declaration has invalid source range? implicit node?
            auto source_range = decls::written_range(decl);
            if(source_range.isValid()) {
                auto [def_fid, definition_range] = unit.produced_range(source_range);
                // A declaration can begin in another file, e.g. when a
                // header-defined macro spells its leading tokens. Such a
                // range is meaningless in this file's coordinates; leave
                // the definition range empty instead of storing it.
                if(fid == def_fid) {
                    relation.set_definition_range(definition_range);
                }
            }
        }

        index->relations[ensure_symbol(decls::normalize(decl))].emplace_back(relation);
    }

    /// The Instantiation relation of an explicit instantiation, at its name
    /// (a macro's invocation that writes none), with the directive's extent.
    void add_instantiation(const clang::ExplicitInstantiationDecl& directive) {
        auto location = unit.context().getSourceManager().getFileLoc(directive.getNameLoc());
        auto [fid, range] = *unit.written_range(location);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }
        Relation relation{.kind = RelationKind::Instantiation, .range = range, .target_symbol = 0};
        if(auto [extent_fid, extent] = unit.produced_range(directive.getSourceRange());
           extent_fid == fid) {
            relation.set_definition_range(extent);
        }
        index->relations[ensure_symbol(decls::normalize(directive.getSpecialization()))]
            .emplace_back(relation);
    }

    /// A symbol-to-symbol relation (type-of, inheritance, overrides, ctor/dtor
    /// ownership); `anchor` decides which file's index receives it.
    void add_pair_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           const clang::NamedDecl* target,
                           clang::SourceRange anchor) {
        // The anchor only routes the relation to a file's index; symbol pairs
        // carry no range of their own.
        auto fid = unit.produced_at(anchor.getBegin()).file;
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        Relation relation{
            .kind = kind,
            .target_symbol = ensure_symbol(decls::normalize(target)),
        };
        index->relations[ensure_symbol(decls::normalize(decl))].emplace_back(relation);
    }

    /// The TypeDefinition relations of a value (a function's: its return
    /// value): the type it is declared with, and for a smart pointer also
    /// the type it points to.
    void add_type_definitions(const clang::ValueDecl* decl) {
        auto* target = type_definition_of(decl, resolver);
        if(!target) {
            return;
        }
        add_pair_relation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
        auto type = types::unwrap(resolver.resolve(value_type(decl)));
        if(!llvm::isa<clang::CXXRecordDecl>(target) || type->isDependentType()) {
            return;
        }
        if(auto* pointed = types::decl_of(types::unwrap(resolver.arrow_pointee(type)))) {
            add_pair_relation(decl, RelationKind::TypeDefinition, pointed, decl->getLocation());
        }
    }

    /// A member of a class template's specialization, or a variable
    /// template's specialization, can have another type than its pattern
    /// declares (`T value` is a Widget in `Box<Widget>`): at a name of one,
    /// a TypeDefinition relation spanning the name holds that type, beside
    /// the pattern's own, which carries no range.
    void add_specialized_type(const clang::NamedDecl* decl, clang::SourceRange name) {
        if(!llvm::isa<clang::FieldDecl, clang::VarDecl, clang::FunctionDecl>(decl)) {
            return;
        }
        auto* pattern = decls::normalize(decl);
        if(pattern == decl->getCanonicalDecl()) {
            return;
        }
        auto* target = type_definition_of(llvm::cast<clang::ValueDecl>(decl), resolver);
        if(!target) {
            return;
        }
        target = decls::normalize(target);
        auto* declared = type_definition_of(llvm::cast<clang::ValueDecl>(pattern), resolver);
        if(declared && unit.get<EntityTable>().entity(decls::normalize(declared)) ==
                           unit.get<EntityTable>().entity(target)) {
            return;
        }
        auto [fid, range] = *unit.written_range(name);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }
        Relation relation{
            .kind = RelationKind::TypeDefinition,
            .range = range,
            .target_symbol = ensure_symbol(target),
        };
        index->relations[ensure_symbol(pattern)].emplace_back(relation);
    }

    /// A call edge, recorded at the call expression's location.
    void add_call_relation(const clang::NamedDecl* decl,
                           RelationKind kind,
                           const clang::NamedDecl* target,
                           clang::SourceRange range) {
        auto [fid, relation_range] = unit.produced_range(range);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }

        Relation relation{
            .kind = kind,
            .range = relation_range,
            .target_symbol = ensure_symbol(decls::normalize(target)),
        };
        index->relations[ensure_symbol(decls::normalize(decl))].emplace_back(relation);
    }

    /// Module names are indexed like macro names: an occurrence plus a
    /// Definition/Declaration/Reference relation keyed by the module's
    /// entity, so navigation flows through the ordinary index pipeline.
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
            auto hash = unit.get<EntityTable>().module_entity(name);

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
                auto [loc_fid, range] =
                    unit.produced_range(clang::SourceRange(import.name_locations.front(),
                                                           import.name_locations.back()));
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
        return result ? result : space_function;
    }

    /// The namespace-scope declaration node `index` lies in, null outside
    /// any. Memoized like enclosing_function.
    const clang::Decl* enclosing_top_level(const Semantics& semantics, std::uint32_t index) {
        auto at_file_scope = [](const clang::DeclContext* context) {
            return context->isFileContext() ||
                   llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context);
        };

        llvm::SmallVector<std::uint32_t> path;
        const clang::Decl* result = nullptr;
        for(auto p = index; p != Semantics::invalid; p = semantics.node(p).parent) {
            if(auto it = top_level_cache.find(p); it != top_level_cache.end()) {
                result = it->second;
                break;
            }
            path.push_back(p);
            if(auto* decl = semantics.node(p).node.get<clang::Decl>()) {
                while(!at_file_scope(decl->getLexicalDeclContext())) {
                    decl = llvm::cast<clang::Decl>(decl->getLexicalDeclContext());
                }
                result = decl;
                break;
            }
        }

        for(auto p: path) {
            top_level_cache.try_emplace(p, result);
        }
        return result;
    }

    /// The include inside `host` that pastes in the fragment holding
    /// `location` (`#include "body.inc"` inside a function); invalid when
    /// `location` lies in `host`'s file itself or nothing inside `host`
    /// includes it.
    clang::SourceLocation pasted_into(const clang::Decl* host, clang::SourceLocation location) {
        auto& SM = unit.context().getSourceManager();
        auto host_file = SM.getFileID(SM.getExpansionLoc(host->getLocation()));
        if(SM.getFileID(location) == host_file) {
            return {};
        }
        while(location.isValid() && SM.getFileID(location) != host_file) {
            location = SM.getIncludeLoc(SM.getFileID(location));
        }
        // Reaching the file is not enough: a redeclaration shares the default
        // arguments its header's declaration wrote.
        auto extent = SM.getExpansionRange(host->getSourceRange());
        if(location.isInvalid() ||
           !SM.isPointWithin(location, extent.getBegin(), extent.getEnd())) {
            return {};
        }
        return location;
    }

    /// Hierarchy ranges are positions in the caller's document: a call
    /// written in a fragment the caller's body pastes in is recorded at
    /// that include.
    clang::SourceRange in_caller_file(const clang::NamedDecl* caller, clang::SourceRange range) {
        auto include =
            pasted_into(caller,
                        unit.context().getSourceManager().getExpansionLoc(range.getBegin()));
        return include.isValid() ? clang::SourceRange(include) : range;
    }

    /// A fragment pasted inside a declaration (an X-macro table included
    /// into a switch) is the pasting file's text: each name it uses gets a
    /// Pasted relation at that include, while its own entries stay where
    /// the fragment spells it.
    void add_pasted_relation(const clang::NamedDecl* decl,
                             const clang::Decl* host,
                             clang::SourceLocation location) {
        if(!host) {
            return;
        }
        auto include = pasted_into(host, location);
        if(include.isInvalid()) {
            return;
        }
        auto [fid, range] = *unit.written_range(include);
        auto* index = file_index(fid);
        if(!index) {
            return;
        }
        Relation relation{.kind = RelationKind::Pasted, .range = range, .target_symbol = 0};
        index->relations[ensure_symbol(decls::normalize(decl))].emplace_back(relation);
    }

    /// Decl-pair relation facts: type definitions, inheritance, overrides,
    /// constructor/destructor ownership and call edges. Only the index
    /// consumes these, so they live here rather than in the semantic layer.
    void project_relations(const Semantics& semantics, std::uint32_t index) {
        const SemanticNode& node = semantics.node(index).node;

        auto call = [&](const clang::NamedDecl* callee, clang::SourceRange range) {
            const clang::NamedDecl* caller = enclosing_function(semantics, index);
            if(caller && callee) {
                range = in_caller_file(caller, range);
                add_call_relation(caller, RelationKind::Callee, callee, range);
                add_call_relation(callee, RelationKind::Caller, caller, range);
            }
        };

        auto& context = unit.context();
        if(auto* CE = node.get<clang::CallExpr>()) {
            // A call no name is written for — the conversion `int i = c`
            // makes — is recorded at the expression it converts.
            auto range = callee_name(CE, context.getSourceManager());
            if(range.isInvalid()) {
                range = CE->getExprLoc();
                if(range.isInvalid()) {
                    return;
                }
            }
            if(auto* callee = llvm::dyn_cast_if_present<clang::NamedDecl>(CE->getCalleeDecl())) {
                call(callee, range);
                return;
            }
            // A dependent call reaches every candidate the resolver finds
            // for it, as its weak references do.
            for(auto* candidate: resolver.lookup(CE)) {
                call(candidate->getUnderlyingDecl(), range);
            }
            return;
        }

        // Constructions are call edges too, but only written ones — an
        // implicit copy or elided temporary has no paren/brace form and
        // would flood the constructor's callers.
        if(auto* CCE = node.get<clang::CXXConstructExpr>()) {
            if(!CCE->getParenOrBraceRange().isValid()) {
                return;
            }
            call(decls::constructor_of(CCE), CCE->getParenOrBraceRange().getBegin());
            return;
        }

        // `a != b` rewritten to `!(a == b)`: the traversal records only the
        // written operands, never the call the rewrite made.
        if(auto* RBO = node.get<clang::CXXRewrittenBinaryOperator>()) {
            if(auto* inner = llvm::dyn_cast_if_present<clang::CXXOperatorCallExpr>(
                   RBO->getDecomposedForm().InnerBinOp)) {
                call(inner->getDirectCallee(), RBO->getOperatorLoc());
            }
            return;
        }

        if(auto* NE = node.get<clang::CXXNewExpr>()) {
            call(NE->getOperatorNew(),
                 keyword_after_scope(context, NE->getBeginLoc(), NE->isGlobalNew()));
            return;
        }

        if(auto* DE = node.get<clang::CXXDeleteExpr>()) {
            auto keyword = keyword_after_scope(context, DE->getBeginLoc(), DE->isGlobalDelete());
            auto* op = DE->getOperatorDelete();
            call(op, keyword);
            // A destroying `operator delete` runs in place of the destructor.
            if(auto type = DE->getDestroyedType();
               !type.isNull() && !(op && op->isDestroyingOperatorDelete())) {
                call(types::destructor_of(type), keyword);
            }
            return;
        }

        auto* D = node.get<clang::Decl>();
        if(!D) {
            return;
        }

        if(auto* EID = llvm::dyn_cast<clang::ExplicitInstantiationDecl>(D)) {
            add_instantiation(*EID);
            return;
        }

        // Recorded where the specialization is written, as base edges
        // are: the template's own entries never change with the units that
        // specialize it.
        if(auto* ND = llvm::dyn_cast<clang::NamedDecl>(D)) {
            if(auto* primary = specialized_template(ND)) {
                add_pair_relation(ND, RelationKind::Primary, primary, ND->getLocation());
                add_pair_relation(primary, RelationKind::Specialization, ND, ND->getLocation());
            }
        }

        // The type of a value declaration, for go-to-type-definition.
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
            add_type_definitions(VD);
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
            if(auto target = types::decl_of(types::unwrap(TND->getUnderlyingType()))) {
                add_pair_relation(TND, RelationKind::TypeDefinition, target, TND->getLocation());
            }
            return;
        }

        // Base/derived edges, recorded at the defining declaration.
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
                        // FIXME: Handle dependent base class.
                        // Through any alias the base is written with: the
                        // hierarchy relates classes.
                        auto target = types::decl_of(base.getType().getCanonicalType());
                        /// A base that is a template parameter (`struct D : T`)
                        /// names no class until instantiation.
                        if(!target ||
                           llvm::isa<clang::TemplateTypeParmDecl, clang::TemplateTemplateParmDecl>(
                               target)) {
                            continue;
                        }
                        add_pair_relation(def, RelationKind::Base, target, base.getSourceRange());
                        add_pair_relation(target,
                                          RelationKind::Derived,
                                          def,
                                          base.getSourceRange());
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

            add_type_definitions(FD);

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

    /// A keyword standing for a type it does not spell — `auto`,
    /// `decltype(auto)`, `decltype(expr)`, an abbreviated template's
    /// `auto` — is an occurrence of the declarations that type names once
    /// its pointers, references and arrays are stripped and a library's
    /// internal aliases seen through (`std::__detail::__unique_ptr_t`), as if
    /// the type were written there, with a Deduced relation: navigation from
    /// the keyword reaches the type, references to the type never list the
    /// keyword.
    void project_placeholder(const Semantics& semantics, std::uint32_t index) {
        const auto& entry = semantics.node(index);
        clang::SourceRange keyword;
        // An abbreviated template's `auto` names a type only once an
        // instantiation substitutes one.
        clang::QualType substituted;
        if(const auto* loc = entry.node.get<clang::TypeLoc>()) {
            if(auto auto_loc = loc->getAs<clang::AutoTypeLoc>()) {
                keyword = auto_loc.isDecltypeAuto()
                              ? clang::SourceRange(auto_loc.getNameLoc(), auto_loc.getRParenLoc())
                              : clang::SourceRange(auto_loc.getNameLoc());
            } else if(auto decltype_loc = loc->getAs<clang::DecltypeTypeLoc>()) {
                keyword = decltype_loc.getDecltypeLoc();
            } else if(auto subst_loc = loc->getAs<clang::SubstTemplateTypeParmTypeLoc>();
                      subst_loc && subst_loc.getTypePtr()->getReplacedParameter()->isImplicit()) {
                keyword = subst_loc.getNameLoc();
                substituted = subst_loc.getTypePtr()->getReplacementType();
            }
        } else if(const auto* function = entry.node.get<clang::FunctionDecl>()) {
            if(const auto* proto = function->getType()->getAs<clang::FunctionProtoType>();
               proto && proto->hasTrailingReturn()) {
                keyword = function->getTypeSpecStartLoc();
            }
        }
        if(keyword.isInvalid() || (entry.flags.in_instantiation && !space)) {
            return;
        }
        auto& SM = unit.context().getSourceManager();
        auto location = SM.getFileLoc(keyword.getBegin());
        if(location != SM.getSpellingLoc(keyword.getBegin())) {
            return;
        }
        // An init-capture's `auto` is implicit, at the captured name.
        auto spelling = unit.token_spelling(location);
        if(spelling != "auto" && spelling != "decltype") {
            return;
        }

        auto owner = index;
        while(!semantics.node(owner).node.get<clang::Decl>()) {
            owner = semantics.node(owner).parent;
        }
        const auto* decl = semantics.node(owner).node.get<clang::Decl>();
        auto type =
            substituted.isNull() ? types::deduced_type(entry.node.dyn_typed(), decl) : substituted;
        if(type.isNull()) {
            return;
        }
        if(const auto* AT = llvm::dyn_cast<clang::AutoType>(type.getTypePtr());
           AT && AT->getDeducedKind() != clang::DeducedKind::Deduced) {
            // A dependent initializer leaves `auto` to instantiation.
            const auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
            if(!var || !var->getInit()) {
                return;
            }
            type = resolver.deduce(var);
            if(type.isNull()) {
                return;
            }
        }

        clang::SourceRange name(location, SM.getFileLoc(keyword.getEnd()));
        for(const auto* target:
            types::decls_of(types::unwrap(readable.readable(type, decl->getDeclContext())),
                            &resolver)) {
            // A closure type is declared by no name to reach.
            if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(target);
               record && record->isLambda()) {
                continue;
            }
            add_occurrence(target, name);
            add_self_relation(target, RelationKind::Deduced, name);
        }
    }

    /// The entries every name occurrence makes: the occurrence where its
    /// token spells the name, and the self-relation spanning the whole
    /// name, whose range it returns.
    clang::SourceRange add_name(const NameOccurrence& occurrence) {
        // Macro-generated names anchor at written source, following
        // clang's file-location rule: argument tokens at their
        // spelling in the invocation, body and pasted tokens at the
        // invocation point. No written entry anchors inside a
        // `#define` body — those tokens have no meaning of their own,
        // each expansion assigns one: what a body names is a resolution of
        // its expansion (project_expansions).
        auto& SM = unit.context().getSourceManager();
        auto location = SM.getFileLoc(occurrence.location);
        auto spelled = location == SM.getSpellingLoc(occurrence.location);
        if(!space && occurrence.location.isMacroID()) {
            body_names.push_back({occurrence.decl, occurrence.kind, occurrence.location});
        }

        // A name of several tokens spans them all where they are
        // written in one file; a macro that spells part of it
        // leaves the first token alone.
        clang::SourceRange name(location);
        if(occurrence.name_end.isValid()) {
            auto end = SM.getFileLoc(occurrence.name_end);
            if(spelled && end == SM.getSpellingLoc(occurrence.name_end) &&
               SM.getFileID(end) == SM.getFileID(location) &&
               SM.getFileOffset(location) <= SM.getFileOffset(end)) {
                name.setEnd(end);
            }
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

        // Every occurrence is mirrored as a self-relation spanning
        // the whole name, so find-references on the occurring decl
        // finds this relation and cursor-site detection finds the
        // occurrence inside it.
        add_self_relation(occurrence.decl, occurrence.kind, name);
        return name;
    }

    /// A header the focus file names something from has its entries in the
    /// preamble's index; a module unit has them only once its own unit is
    /// indexed. Until then, its declarations of what the focus file names
    /// come from the AST the import deserialized, as that unit's index
    /// records them.
    void project_imported() {
        llvm::SmallVector<NameOccurrence> declarations;
        for(const auto* decl: imported) {
            for(const auto* redecl: decl->redecls()) {
                if(!redecl->isFromASTFile() || !redecl->isInNamedModule()) {
                    continue;
                }
                for(auto& reference: resolve_references(SemanticNode(redecl))) {
                    if(reference.location.isValid() && reference.kind.isDeclOrDef()) {
                        declarations.push_back({reference.decl,
                                                reference.kind,
                                                reference.location,
                                                reference.name_end});
                        module_files.insert(unit.seen_at(reference.location).file);
                    }
                }
            }
        }
        for(auto& declaration: declarations) {
            add_name(declaration);
        }
    }

    void project_node(const Semantics& semantics, std::uint32_t i) {
        // What an instantiation resolves are the calls and member accesses
        // its body's expressions make and the types its placeholders
        // deduce; it declares nothing its template does not, and the types
        // it substitutes are no references.
        if(space && !semantics.node(i).node.get<clang::Stmt>()) {
            project_placeholder(semantics, i);
            if(const auto* value = semantics.node(i).node.get<clang::ValueDecl>()) {
                add_deduced_type(value);
            }
            return;
        }
        for(auto& occurrence: resolve_occurrences(semantics, i, &resolver)) {
            auto name = add_name(occurrence);
            if(space) {
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
            if(occurrence.kind.is_one_of(RelationKind::Reference, RelationKind::WeakReference) &&
               names_member_qualified(semantics.node(i).node)) {
                if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(occurrence.decl);
                   method && method->isVirtual()) {
                    add_self_relation(occurrence.decl, RelationKind::Direct, name);
                }
            }

            if(!occurrence.kind.is_one_of(RelationKind::Declaration,
                                          RelationKind::Definition,
                                          RelationKind::WeakReference)) {
                add_pasted_relation(occurrence.decl,
                                    enclosing_top_level(semantics, i),
                                    name.getBegin());
            }
        }

        project_placeholder(semantics, i);
        project_relations(semantics, i);
    }

    /// In an instantiation, a variable or a parameter its template declares
    /// with a placeholder type (`auto x`, an abbreviated template's `auto`
    /// parameter): the types go-to-type-definition reaches from it, at its
    /// name — beside the template's own, which carries none.
    void add_deduced_type(const clang::ValueDecl* decl) {
        if(!llvm::isa<clang::VarDecl>(decl)) {
            return;
        }
        const auto* pattern = llvm::cast<clang::ValueDecl>(in_pattern(decl));
        auto declared = pattern->getType();
        const auto* parameter = types::unwrap(declared)->getAs<clang::TemplateTypeParmType>();
        if(pattern == decl ||
           (!declared->getContainedDeducedType() &&
            !(parameter && parameter->getDecl() && parameter->getDecl()->isImplicit()))) {
            return;
        }
        auto type = types::unwrap(value_type(decl));
        auto* target = types::decl_of(type);
        // A name a macro body spells is written nowhere.
        auto written = unit.written_range(decl->getLocation());
        if(!target || !written) {
            return;
        }
        auto [fid, range] = *written;
        auto* index = file_index(fid);
        auto symbol = ensure_symbol(decls::normalize(decl));
        auto add = [&](const clang::NamedDecl* type_decl) {
            Relation relation{.kind = RelationKind::TypeDefinition,
                              .range = range,
                              .target_symbol = ensure_symbol(decls::normalize(type_decl))};
            index->relations[symbol].emplace_back(relation);
        };
        add(target);
        // A smart pointer leads on to its pointee, as add_type_definitions.
        if(llvm::isa<clang::CXXRecordDecl>(target)) {
            if(auto* pointed = types::decl_of(types::unwrap(resolver.arrow_pointee(type)))) {
                add(pointed);
            }
        }
    }

    /// The pattern's own declaration of a parameter or a local an
    /// instantiated body declares: the definition's parameter at the same
    /// position — a specialization takes its parameters from whichever
    /// declaration lookup met (`fwd.h` redeclaring the template) — and the
    /// local whose location instantiation copied.
    const clang::NamedDecl* in_pattern(const clang::NamedDecl* decl) {
        const auto* function =
            llvm::dyn_cast_if_present<clang::FunctionDecl>(decl->getParentFunctionOrMethod());
        const auto* pattern = function ? function->getTemplateInstantiationPattern() : nullptr;
        if(!pattern || pattern == function) {
            return decl;
        }
        if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(decl);
           param && param->getDeclContext() == function) {
            const auto* definition = pattern->getDefinition();
            const auto* source = definition ? definition : pattern;
            auto index = param->getFunctionScopeIndex();
            return index < source->getNumParams() ? source->getParamDecl(index) : decl;
        }
        auto key = [](const clang::NamedDecl* named) {
            return std::pair(named->getLocation().getRawEncoding(),
                             static_cast<unsigned>(named->getKind()));
        };
        auto [it, fresh] = pattern_locals.try_emplace(pattern);
        if(fresh) {
            // A member template's body can be written outside its class,
            // in a redeclaration of the pattern.
            for(const auto* redecl: pattern->redecls()) {
                for(const auto* local: redecl->decls()) {
                    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(local)) {
                        it->second.try_emplace(key(named), named);
                    }
                }
            }
        }
        auto found = it->second.find(key(decl));
        return found != it->second.end() ? found->second : decl;
    }

    /// The file holding the definition an instantiation was made from: what
    /// it resolves lies there. The pattern clang answers for a function or
    /// a variable can be a declaration the unit's lookup met (`std::min`
    /// redeclared in algorithmfwd.h after its definition), which varies
    /// with the unit; the definition does not.
    clang::FileID home_of(const clang::Decl* decl) {
        const clang::Decl* pattern = nullptr;
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            if(const auto* found = function->getTemplateInstantiationPattern()) {
                const auto* definition = found->getDefinition();
                pattern = definition ? definition : found;
            }
        } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
            if(const auto* found = var->getTemplateInstantiationPattern()) {
                const auto* definition = found->getDefinition();
                pattern = definition ? definition : found;
            }
        } else if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
            pattern = record->getTemplateInstantiationPattern();
        }
        auto& SM = unit.context().getSourceManager();
        return SM.getFileID(SM.getFileLoc((pattern ? pattern : decl)->getLocation()));
    }

    /// Whether a file's instantiations belong to this run's index: not to
    /// a borrowed includer context, whose host indexes them.
    bool resolves_in(clang::FileID fid) {
        return fid.isValid() && !unit.from_context(fid);
    }

    /// The function and variable instantiations to walk; the known ones a
    /// file holds go to its skipped keys.
    std::vector<const clang::Decl*> walked_instantiations() {
        std::vector<const clang::Decl*> walked;
        for(const auto* decl: unit.instantiations()) {
            if(llvm::isa<clang::CXXRecordDecl>(decl) ||
               (!standard_library && decls::in_standard_library(decl))) {
                continue;
            }
            auto home = home_of(decl);
            if(!resolves_in(home)) {
                continue;
            }
            auto key = unit.get<EntityTable>().entity(llvm::cast<clang::NamedDecl>(decl));
            if(std::ranges::binary_search(known_instantiations, key)) {
                resolved[home].skipped.push_back(key);
                continue;
            }
            walked.push_back(decl);
        }
        return walked;
    }

    /// The written relations of one file, by what a record would say: an
    /// instantiation resolves only what its template's own entries leave
    /// open there. Empty for a file this run does not index — a Content
    /// compile's headers, whose records then stand alone.
    using WrittenKey =
        std::tuple<SymbolHash, std::uint32_t, std::uint32_t, std::uint32_t, SymbolHash>;

    static WrittenKey written_key(SymbolHash symbol, const Relation& relation) {
        // A weak reference already names what the reference would.
        RelationKind kind = relation.kind;
        auto kind_class = kind.isReference() ? RelationKind::Reference : kind.value();
        return {symbol,
                kind_class,
                relation.range.begin,
                relation.range.end,
                kind.carries_extent() ? 0 : relation.target_symbol};
    }

    const llvm::DenseSet<WrittenKey>& written_in(clang::FileID fid) {
        auto [it, fresh] = written.try_emplace(fid);
        if(fresh) {
            if(auto found = file_indices.find(fid); found != file_indices.end()) {
                for(auto& [symbol, relations]: found->second.relations) {
                    for(auto& relation: relations) {
                        it->second.insert(written_key(symbol, relation));
                    }
                }
            }
        }
        return it->second;
    }

    /// The records one instantiation's entries in its home file amount to,
    /// beyond what the written entries there say.
    std::vector<InstantiationRecord> records_of(clang::FileID home, const FileIndex& entries) {
        const auto& known = written_in(home);
        std::vector<InstantiationRecord> records;
        for(auto& [symbol, relations]: entries.relations) {
            for(auto& relation: relations) {
                RelationKind kind = relation.kind;
                InstantiationRecord record{.range = relation.range, .target = symbol};
                if(kind.isReference()) {
                    record.kind = InstantiationKind::Reference;
                } else if(kind.is_one_of(RelationKind::Deduced)) {
                    record.kind = InstantiationKind::Deduced;
                } else if(kind.is_one_of(RelationKind::Implied)) {
                    record.kind = InstantiationKind::Implied;
                } else if(kind.is_one_of(RelationKind::Callee)) {
                    record.kind = InstantiationKind::Call;
                    record.target = relation.target_symbol;
                    record.from = symbol;
                } else if(kind.is_one_of(RelationKind::TypeDefinition) && relation.range.valid()) {
                    record.kind = InstantiationKind::TypeDefinition;
                    record.target = relation.target_symbol;
                    record.from = symbol;
                } else {
                    continue;
                }
                if(!known.contains(written_key(symbol, relation))) {
                    records.push_back(record);
                }
            }
        }
        return records;
    }

    /// One walked instantiation: its key in its home file, and the resolution
    /// its records make there, if any.
    void add_walked(clang::FileID home,
                    const clang::NamedDecl* decl,
                    std::vector<InstantiationRecord> records) {
        auto& file = resolved[home].walked;
        FileInstantiations::Key key{
            .key = unit.get<EntityTable>().entity(decl),
            .pattern = ensure_symbol(decls::normalize(decl)),
        };
        if(!records.empty()) {
            key.resolution = static_cast<std::uint32_t>(file.resolutions.size());
            file.resolutions.push_back(std::move(records));
        }
        file.keys.push_back(key);
    }

    void project_instantiations(const Semantics& semantics) {
        // The memos are by node index, which a focus compile's build of
        // its instantiations numbers anew.
        enclosing_cache.clear();
        top_level_cache.clear();
        for(const auto& root: semantics.instantiation_roots()) {
            const auto* decl = llvm::cast<clang::NamedDecl>(root.decl);
            auto home = home_of(decl);
            llvm::DenseMap<clang::FileID, FileIndex> entries;
            space = &entries;
            space_function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            for(auto i = root.begin; i < root.end; i += 1) {
                if(semantics.node(i).node.is_ast()) {
                    project_node(semantics, i);
                }
            }
            space = nullptr;
            space_function = nullptr;
            std::vector<InstantiationRecord> records;
            if(auto it = entries.find(home); it != entries.end()) {
                records = records_of(home, it->second);
            }
            // A body that includes a fragment (`#include "Instruction.def"`
            // in a switch) resolves names in the fragment's file too, which
            // a skipped key could not stand for: such an instantiation keeps
            // no key, every unit walks it, and each file holds its records
            // as a resolution of its own.
            bool spilled = false;
            for(auto& [fid, index]: entries) {
                if(fid == home || !resolves_in(fid)) {
                    continue;
                }
                if(auto fragment = records_of(fid, index); !fragment.empty()) {
                    resolved[fid].walked.resolutions.push_back(std::move(fragment));
                    spilled = true;
                }
            }
            // Nor can a skipped key stand for a name below external scope
            // that another file declares: only the units walking the
            // instantiation list the key's file among the files naming it
            // (TUManifest::local_fanout).
            auto foreign = [&](SymbolHash hash) {
                auto it = internal_files.find(hash);
                return it != internal_files.end() && it->second != home;
            };
            bool names_foreign = llvm::any_of(records, [&](const InstantiationRecord& record) {
                return foreign(record.target) || (has_from(record.kind) && foreign(record.from));
            });
            if(!spilled && !names_foreign) {
                add_walked(home, decl, std::move(records));
            } else if(!records.empty()) {
                resolved[home].walked.resolutions.push_back(std::move(records));
            }
        }
    }

    /// A class instantiation's bases, where its template's base specifiers
    /// depend on the arguments (`struct D : T`): no key, the resolution alone —
    /// computing them costs less than looking one up.
    void project_bases(const clang::CXXRecordDecl* record) {
        const auto* pattern = record->getTemplateInstantiationPattern();
        if(!pattern || !record->hasDefinition() ||
           (!standard_library && decls::in_standard_library(record)) ||
           llvm::none_of(pattern->bases(), [](const clang::CXXBaseSpecifier& base) {
               return base.getType()->isDependentType();
           })) {
            return;
        }
        auto home = home_of(record);
        if(!resolves_in(home)) {
            return;
        }
        const auto& known = written_in(home);
        auto derived = ensure_symbol(decls::normalize(record));
        std::vector<InstantiationRecord> records;
        for(const auto& base: record->bases()) {
            const auto* target = types::decl_of(base.getType().getCanonicalType());
            if(!target) {
                continue;
            }
            auto written =
                unit.written_range(base.getTypeSourceInfo()->getTypeLoc().getSourceRange());
            if(!written || written->file != home) {
                continue;
            }
            auto symbol = ensure_symbol(decls::normalize(target));
            Relation relation{.kind = RelationKind::Base, .target_symbol = symbol};
            if(known.contains(written_key(derived, relation))) {
                continue;
            }
            records.push_back({.range = written->range,
                               .kind = InstantiationKind::Base,
                               .target = symbol,
                               .from = derived});
        }
        if(!records.empty()) {
            resolved[home].walked.resolutions.push_back(std::move(records));
        }
    }

    /// What one expansion's `#define` body spells — the names of the
    /// declarations and macros it reaches, in the body's text — a resolution
    /// of the macro's file without a key: the same body resolves differently
    /// from one expansion to the next (`LLVM_DEBUG` and each file's
    /// `DEBUG_TYPE`).
    void project_expansions() {
        auto& SM = unit.context().getSourceManager();
        auto& entities = unit.get<EntityTable>();

        // The standard library's own macros: a system header's, by a name
        // reserved for the implementation (`__glibcxx_assert`).
        auto library_macro = [&](clang::SourceLocation expansion, clang::SourceLocation body) {
            if(!SM.isInSystemHeader(body)) {
                return false;
            }
            auto name = clang::Lexer::getImmediateMacroName(expansion, SM, unit.lang_options());
            return clang::isReservedInAllContexts(
                unit.context().Idents.get(name).isReserved(unit.lang_options()));
        };

        // By the expansion the body token came out of; an argument or a
        // pasted token is spelled in no body.
        llvm::MapVector<clang::FileID, std::pair<clang::FileID, std::vector<InstantiationRecord>>>
            expansions;
        auto add = [&](clang::SourceLocation location, SymbolHash target, InstantiationKind kind) {
            auto expansion = location;
            while(location.isMacroID()) {
                expansion = location;
                location = SM.getImmediateSpellingLoc(location);
            }
            auto& info = SM.getSLocEntry(SM.getFileID(expansion)).getExpansion();
            if(info.isMacroArgExpansion() || SM.isWrittenInScratchSpace(location) ||
               (!standard_library && library_macro(expansion, location))) {
                return;
            }
            auto [fid, offset] = SM.getDecomposedLoc(location);
            if(!resolves_in(fid)) {
                return;
            }
            auto length = clang::Lexer::MeasureTokenLength(location, SM, unit.lang_options());
            auto& [home, records] = expansions[SM.getFileID(expansion)];
            home = fid;
            records.push_back({
                .range = {offset, offset + length},
                .kind = kind,
                .target = target,
                .expansion = true
            });
        };
        for(auto& directive: llvm::make_second_range(unit.directives())) {
            for(auto& macro: directive.macros) {
                if(macro.kind == MacroRef::Ref && macro.loc.isMacroID()) {
                    add(macro.loc, entities.entity(macro.macro), InstantiationKind::Reference);
                }
            }
        }
        // What a body declares is each expansion's own: its uses alone.
        for(auto& name: body_names) {
            std::optional<InstantiationKind> kind;
            if(name.kind.isReference()) {
                kind = InstantiationKind::Reference;
            } else if(name.kind.is_one_of(RelationKind::Deduced)) {
                kind = InstantiationKind::Deduced;
            } else if(name.kind.is_one_of(RelationKind::Implied)) {
                kind = InstantiationKind::Implied;
            }
            if(kind) {
                add(name.location, ensure_symbol(decls::normalize(name.decl)), *kind);
            }
        }
        for(auto& [home, records]: llvm::make_second_range(expansions)) {
            resolved[home].walked.resolutions.push_back(std::move(records));
        }
    }

    void project_semantics(const Semantics& semantics) {
        auto entries = semantics.node_entries();
        auto roots = semantics.instantiation_roots();
        auto written_end = roots.empty() ? entries.size() : roots.front().begin;
        for(std::uint32_t i = 0; i < written_end; i += 1) {
            // Macros are projected from the directives below; includes and
            // imports have their own pipelines.
            if(entries[i].node.is_ast()) {
                project_node(semantics, i);
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
        }
    }

    std::string build(const PreambleExtras* extras) {
        ScopedTimer semantics_timer;
        auto walked = walked_instantiations();
        // The focus-only shape is the one features share, cached on the
        // unit; the whole-TU shape is transient — projected and dropped.
        // Both phases below share the one build. A focus compile walks its
        // instantiations in a build of their own.
        std::optional<Semantics> full;
        if(!focus_only) {
            full.emplace(Semantics::build(unit, {.all_files = true, .instantiations = walked}));
        } else if(!walked.empty()) {
            full.emplace(Semantics::build(
                unit,
                {.all_files = true, .instantiations = walked, .only_instantiations = true}));
        }
        const Semantics& semantics = focus_only ? unit.get<Semantics>() : *full;
        auto semantics_ms = semantics_timer.ms_f();

        ScopedTimer project_timer;
        project_semantics(semantics);

        index_modules(semantics);
        project_imported();
        auto project_ms = project_timer.ms_f();

        ScopedTimer instantiations_timer;
        if(full) {
            project_instantiations(*full);
        }
        for(const auto* decl: unit.instantiations()) {
            if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
                project_bases(record);
            }
        }
        // What a preamble instantiates and expands is the background
        // index's to resolve: the preamble's own index carries none of it.
        if(!extras) {
            project_expansions();
        }
        auto instantiations_ms = instantiations_timer.ms_f();

        ScopedTimer finish_timer;
        // Build the include graph from what the index actually recorded:
        // every fid keying `file_indices` gets its include chain resolved
        // through the SourceManager, so the lookups below cannot miss.
        llvm::SmallVector<clang::FileID, 16> indexed_fids;
        indexed_fids.reserve(file_indices.size() + resolved.size());
        for(auto& [fid, index]: file_indices) {
            indexed_fids.push_back(fid);
        }
        for(auto fid: llvm::make_first_range(resolved)) {
            if(!file_indices.contains(fid)) {
                indexed_fids.push_back(fid);
            }
        }
        // The unit writes no entries for what it reads through the
        // precompiled header or a module, yet an internal name declared
        // there is named in the declaring files as much as in the unit's own.
        auto& SM = unit.context().getSourceManager();
        llvm::SmallVector<std::pair<SymbolHash, clang::FileID>> declaring_files;
        for(auto [hash, decl]: precompiled_internal) {
            for(const auto* redecl: decl->redecls()) {
                auto fid = SM.getFileID(SM.getFileLoc(redecl->getLocation()));
                if(redecl->isFromASTFile() && fid.isValid() && !unit.from_context(fid)) {
                    declaring_files.emplace_back(hash, fid);
                    indexed_fids.push_back(fid);
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

        for(auto& file_index: llvm::make_second_range(by_path)) {
            drop_nested(file_index.occurrences);
        }

        // Instantiations by path alike: what each FileID of a header resolves
        // is one set of resolutions and keys.
        // A `#define` of the focus file's preamble lies in the PCH's copy of
        // the file, at the offsets the buffer has.
        llvm::DenseMap<std::uint32_t, Resolved> resolved_by_path;
        for(auto& [fid, file]: resolved) {
            auto home = fid == SM.getPreambleFileID() ? unit.focus_file().file() : fid;
            if(home != unit.focus_file().file() && tree.node_of(home) == no_node) {
                continue;
            }
            auto path_id = tree.path_id(home);
            path_fids.try_emplace(path_id, home);
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
            llvm::append_range(into.skipped, file.skipped);
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

        // The canonical file is decided across every file's relations: a
        // definition wins over a declaration, and between equals the
        // lowest path id, so the choice is a pure function of the relations.
        auto adopt = [](Symbol& symbol, std::uint32_t path_id, bool definition) {
            bool defined = has_flag(symbol.flags, SymbolFlags::HasDefinition);
            if(definition && !defined) {
                symbol.flags |= SymbolFlags::HasDefinition;
                symbol.file = path_id;
                return;
            }
            if(definition == defined && (symbol.file == no_file || path_id < symbol.file)) {
                symbol.file = path_id;
            }
        };
        for(auto& [path_id, file_index]: by_path) {
            for(auto& [symbol_id, relations]: file_index.relations) {
                auto& symbol = symbols[symbol_id];
                symbol.reference_files.add(path_id);
                for(auto& relation: relations) {
                    if(relation.kind == RelationKind::Definition) {
                        adopt(symbol, path_id, true);
                    } else if(relation.kind == RelationKind::Declaration) {
                        adopt(symbol, path_id, false);
                    } else if(RelationKind(relation.kind).isBetweenSymbol()) {
                        // A query anchors a relation's target at the file
                        // holding the relation; an internal one reaches its
                        // own entries from there only through this file.
                        if(auto it = symbols.find(relation.target_symbol);
                           it != symbols.end() && it->second.scope != SymbolScope::External) {
                            it->second.reference_files.add(path_id);
                        }
                    }
                }
            }
        }
        auto finish_ms = finish_timer.ms_f();

        ScopedTimer encode_timer;
        auto resolve = [&](SymbolHash hash) -> std::optional<SymbolIdentity> {
            auto it = symbols.find(hash);
            if(it == symbols.end()) {
                return std::nullopt;
            }
            return it->second.identity();
        };

        llvm::SmallVector<std::uint32_t> path_ids;
        path_ids.reserve(by_path.size());
        for(auto path_id: llvm::make_first_range(by_path)) {
            path_ids.push_back(path_id);
        }
        llvm::sort(path_ids);
        std::vector<FileSection> sections;
        llvm::BitVector known_paths(tree.paths.size());
        for(auto path_id: path_ids) {
            auto& file_index = by_path[path_id];
            if(file_index.empty()) {
                continue;
            }
            std::string bytes;
            llvm::raw_string_ostream os(bytes);
            write_shard(file_index, resolve, unit.file_content(path_fids[path_id]), os);
            auto hash = llvm::xxh3_64bits(bytes);
            if(std::ranges::binary_search(known_variants, hash)) {
                known_paths.set(path_id);
                sections.push_back({path_id, hash, {}});
                continue;
            }
            sections.push_back(
                {path_id, hash, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
        }
        std::vector<InstantiationSection> instantiation_sections;
        llvm::SmallVector<std::uint32_t> resolved_paths(llvm::make_first_range(resolved_by_path));
        llvm::sort(resolved_paths);
        for(auto path_id: resolved_paths) {
            auto& file = resolved_by_path[path_id];
            InstantiationSection section{.path_id = path_id};
            if(!file.walked.empty()) {
                auto content = unit.file_content(path_fids[path_id]);
                std::string bytes;
                llvm::raw_string_ostream os(bytes);
                write_instantiations(file.walked,
                                     llvm::xxh3_64bits(content),
                                     static_cast<std::uint32_t>(content.size()),
                                     os);
                section.blob.assign(bytes.begin(), bytes.end());
            }
            llvm::sort(file.skipped);
            file.skipped.erase(llvm::unique(file.skipped), file.skipped.end());
            section.skipped = std::move(file.skipped);
            instantiation_sections.push_back(std::move(section));
        }
        auto encode_ms = encode_timer.ms_f();

        TUIndexBlob blob;
        blob.format_version = index_format_version;
        blob.built_at = unit.build_at().count();
        blob.paths = std::move(tree.paths);
        blob.path_hashes = std::move(tree.path_hashes);
        blob.nodes = std::move(tree.nodes);
        blob.absent = unit.absent();
        llvm::SmallVector<SymbolHash> table;
        for(auto& [hash, symbol]: symbols) {
            // A function's own name stays out but where an instantiation or
            // an expansion names it in another file, which the merge lists
            // (TUIndex::local_fanout).
            if(symbol.scope == SymbolScope::FileLocal &&
               (focus_only || symbol.reference_files.cardinality() < 2)) {
                continue;
            }
            if(symbol.scope == SymbolScope::External && !symbol.reference_files.isEmpty() &&
               symbol.instantiation_files.isEmpty() &&
               llvm::all_of(symbol.reference_files,
                            [&](std::uint32_t path_id) { return known_paths.test(path_id); })) {
                continue;
            }
            table.push_back(hash);
        }
        llvm::sort(table);
        for(auto hash: table) {
            auto& symbol = symbols.find(hash)->second;
            blob.sym_hashes.push_back(hash);
            blob.sym_names += symbol.name;
            blob.sym_name_ends.push_back(static_cast<std::uint32_t>(blob.sym_names.size()));
            blob.sym_args += symbol.args;
            blob.sym_args_ends.push_back(static_cast<std::uint32_t>(blob.sym_args.size()));
            blob.sym_signatures += symbol.signature;
            blob.sym_signature_ends.push_back(
                static_cast<std::uint32_t>(blob.sym_signatures.size()));
            blob.sym_parents.push_back(symbol.parent);
            blob.sym_kinds.push_back(symbol.kind.value());
            blob.sym_scopes.push_back(static_cast<std::uint8_t>(symbol.scope));
            blob.sym_flags.push_back(static_cast<std::uint16_t>(symbol.flags));
            blob.sym_token_kinds.push_back(static_cast<std::uint8_t>(symbol.token_class.kind));
            blob.sym_token_modifiers.push_back(symbol.token_class.modifiers);
            blob.sym_files.push_back(symbol.file);
            for(auto file: symbol.reference_files) {
                blob.sym_references.push_back(file);
            }
            blob.sym_reference_ends.push_back(
                static_cast<std::uint32_t>(blob.sym_references.size()));
            for(auto file: symbol.instantiation_files) {
                blob.sym_instantiations.push_back(file);
            }
            blob.sym_instantiation_ends.push_back(
                static_cast<std::uint32_t>(blob.sym_instantiations.size()));
        }
        blob.sections = std::move(sections);
        blob.instantiations = std::move(instantiation_sections);
        if(extras) {
            blob.preamble_hash = extras->hash;
            blob.preamble_size = extras->size;
            blob.links = extras->links;
            blob.inactive_regions = extras->inactive_regions;
            blob.open_conditionals = extras->open_conditionals;
            blob.diagnostics = extras->diagnostics.str();
        }

        ScopedTimer pack_timer;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        serialize_blob(blob, os);

        LOG_PERF("index_detail",
                 "op=build scope={} semantics_ms={:.2f} project_ms={:.2f} "
                 "instantiations_ms={:.2f} instantiations={} walked={} finish_ms={:.2f} "
                 "encode_ms={:.2f} pack_ms={:.2f}",
                 focus_only ? "focus" : "full",
                 semantics_ms,
                 project_ms,
                 instantiations_ms,
                 unit.instantiations().size(),
                 walked.size(),
                 finish_ms,
                 encode_ms,
                 pack_timer.ms_f());
        return bytes;
    }

private:
    CompilationUnit& unit;
    bool focus_only;
    llvm::ArrayRef<std::uint64_t> known_variants;
    types::TemplateResolver resolver;
    /// The types function signatures print as readers know them.
    display::ReadableTypes readable;
    IncludeTree tree;
    SymbolTable symbols;
    /// Build-time working state keyed by FileID — clang::FileID means
    /// nothing outside the compilation, so it never leaves the builder;
    /// the encode step converts it through tree.path_id.
    llvm::DenseMap<clang::FileID, FileIndex> file_indices;
    /// With focus_only, the symbols registered that an imported
    /// module declares, a namespace aside: every unit reopening it
    /// redeclares it.
    llvm::SmallVector<const clang::NamedDecl*> imported;
    /// The file declaring each symbol registered below external scope.
    llvm::DenseMap<SymbolHash, clang::FileID> internal_files;
    /// The internal-linkage symbols registered that the precompiled header
    /// or an imported module declares.
    llvm::SmallVector<std::pair<SymbolHash, const clang::NamedDecl*>> precompiled_internal;
    llvm::DenseSet<clang::FileID> module_files;
    llvm::DenseMap<std::uint32_t, const clang::NamedDecl*> enclosing_cache;
    llvm::DenseMap<std::uint32_t, const clang::Decl*> top_level_cache;

    /// The keys of the instantiations the receiver already holds
    /// (TUIndexOptions::known_instantiations).
    llvm::ArrayRef<std::uint64_t> known_instantiations;

    /// Whether the standard library's own instantiations and expansions are
    /// walked (TUIndexOptions::standard_library_instantiations).
    bool standard_library;

    /// What this run's instantiations and macro expansions resolve in each
    /// file, and the known keys whose definition the file holds.
    struct Resolved {
        FileInstantiations walked;
        std::vector<SymbolHash> skipped;
    };

    llvm::DenseMap<clang::FileID, Resolved> resolved;

    /// Per file, its written relations as records would say them
    /// (written_in).
    llvm::DenseMap<clang::FileID, llvm::DenseSet<WrittenKey>> written;

    /// While an instantiation's nodes project, the entries they make:
    /// file_index hands these out instead of the files' own. The
    /// definition, when it is a function, is the caller of its calls.
    llvm::DenseMap<clang::FileID, FileIndex>* space = nullptr;
    const clang::FunctionDecl* space_function = nullptr;

    /// Per pattern function, its parameters and locals by location and kind.
    llvm::DenseMap<
        const clang::FunctionDecl*,
        llvm::DenseMap<std::pair<clang::SourceLocation::UIntTy, unsigned>, const clang::NamedDecl*>>
        pattern_locals;

    /// The names the written code spells through a macro: the ones a
    /// `#define` body spells make the expansions' resolutions.
    struct BodyName {
        const clang::NamedDecl* decl;
        RelationKind kind;
        clang::SourceLocation location;
    };

    std::vector<BodyName> body_names;
};

}  // namespace

std::string build_tu_index(CompilationUnit& unit, const TUIndexOptions& options) {
    Projector projector(unit, options);
    return projector.build(nullptr);
}

std::string build_preamble_index(CompilationUnit& unit,
                                 llvm::ArrayRef<DocumentLink> links,
                                 llvm::ArrayRef<std::uint32_t> inactive_regions,
                                 llvm::ArrayRef<std::uint8_t> open_conditionals,
                                 llvm::StringRef diagnostics) {
    // The preamble compile remaps the buffer truncated at the bound, so
    // the focus file's content is exactly the preamble text the PCH was
    // built from.
    auto preamble_text = unit.focus_file().content();
    PreambleExtras extras{
        .hash = llvm::xxh3_64bits(preamble_text),
        .size = static_cast<std::uint32_t>(preamble_text.size()),
        .links = links,
        .inactive_regions = inactive_regions,
        .open_conditionals = open_conditionals,
        .diagnostics = diagnostics,
    };
    Projector projector(unit, {});
    return projector.build(&extras);
}

namespace {

using TUIndexView = kota::codec::fbs::table_view<TUIndexBlob>;

/// The buffer was fully verified at TUIndex::from_bytes; per-accessor
/// views skip that cost.
TUIndexView blob_root(llvm::StringRef data) {
    return TUIndexView::from_verified_bytes(blob_bytes(data));
}

/// The symbol table columns of a TU index blob, borrowing its bytes.
struct SymbolColumns {
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::StringRef names;
    llvm::ArrayRef<std::uint32_t> name_ends;
    llvm::StringRef args;
    llvm::ArrayRef<std::uint32_t> args_ends;
    llvm::StringRef signatures;
    llvm::ArrayRef<std::uint32_t> signature_ends;
    llvm::ArrayRef<std::uint64_t> parents;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint8_t> scopes;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint8_t> token_kinds;
    llvm::ArrayRef<std::uint32_t> token_modifiers;
    llvm::ArrayRef<std::uint32_t> files;
    llvm::ArrayRef<std::uint32_t> reference_ends;
    llvm::ArrayRef<std::uint32_t> references;
    llvm::ArrayRef<std::uint32_t> instantiation_ends;
    llvm::ArrayRef<std::uint32_t> instantiations;

    static SymbolColumns of(TUIndexView root) {
        return {
            .hashes = to_array_ref(root[&TUIndexBlob::sym_hashes]),
            .names = to_ref(root[&TUIndexBlob::sym_names]),
            .name_ends = to_array_ref(root[&TUIndexBlob::sym_name_ends]),
            .args = to_ref(root[&TUIndexBlob::sym_args]),
            .args_ends = to_array_ref(root[&TUIndexBlob::sym_args_ends]),
            .signatures = to_ref(root[&TUIndexBlob::sym_signatures]),
            .signature_ends = to_array_ref(root[&TUIndexBlob::sym_signature_ends]),
            .parents = to_array_ref(root[&TUIndexBlob::sym_parents]),
            .kinds = to_array_ref(root[&TUIndexBlob::sym_kinds]),
            .scopes = to_array_ref(root[&TUIndexBlob::sym_scopes]),
            .flags = to_array_ref(root[&TUIndexBlob::sym_flags]),
            .token_kinds = to_array_ref(root[&TUIndexBlob::sym_token_kinds]),
            .token_modifiers = to_array_ref(root[&TUIndexBlob::sym_token_modifiers]),
            .files = to_array_ref(root[&TUIndexBlob::sym_files]),
            .reference_ends = to_array_ref(root[&TUIndexBlob::sym_reference_ends]),
            .references = to_array_ref(root[&TUIndexBlob::sym_references]),
            .instantiation_ends = to_array_ref(root[&TUIndexBlob::sym_instantiation_ends]),
            .instantiations = to_array_ref(root[&TUIndexBlob::sym_instantiations]),
        };
    }

    std::optional<std::uint32_t> find(SymbolHash hash) const {
        auto it = std::ranges::lower_bound(hashes, hash);
        if(it == hashes.end() || *it != hash) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - hashes.begin());
    }

    SymbolIdentity identity(std::uint32_t entry) const {
        return {
            .name = back_to_back(names, name_ends, entry),
            .args = back_to_back(args, args_ends, entry),
            .signature = back_to_back(signatures, signature_ends, entry),
            .parent = parents[entry],
            .kind = SymbolKind(kinds[entry]),
            .scope = static_cast<SymbolScope>(scopes[entry]),
            .flags = static_cast<SymbolFlags>(flags[entry]),
            .file = files[entry],
            .token_class = {.kind = static_cast<SemanticTokenKind>(token_kinds[entry]),
                            .modifiers = token_modifiers[entry]},
        };
    }

    llvm::ArrayRef<std::uint32_t> references_of(std::uint32_t entry) const {
        return back_to_back(references, reference_ends, entry);
    }

    llvm::ArrayRef<std::uint32_t> instantiations_of(std::uint32_t entry) const {
        return back_to_back(instantiations, instantiation_ends, entry);
    }
};

}  // namespace

TUIndex TUIndex::from_bytes(llvm::StringRef data) {
    auto root = TUIndexView::from_bytes(blob_bytes(data));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting TU index: structural verification failed");
        return {};
    }
    if(root[&TUIndexBlob::format_version] != index_format_version) {
        LOG_DEBUG("Rejecting TU index: format version {}, this build reads {}",
                  root[&TUIndexBlob::format_version],
                  index_format_version);
        return {};
    }

    // Structural verification does not constrain field values; every path
    // id the merge dereferences against the path table is bounded here so
    // the accessors stay check-free. The builder ends every path table
    // with the focus file, so consumers address path_count() - 1
    // unchecked — an empty table marks a corrupt TU index.
    auto count = root[&TUIndexBlob::paths].size();
    if(count == 0) {
        LOG_DEBUG("Rejecting TU index: empty path table");
        return {};
    }
    // A parent names another node or the root; consumers index their
    // per-node tables with it unchecked.
    auto nodes = root[&TUIndexBlob::nodes];
    for(std::size_t i = 0; i < nodes.size(); i += 1) {
        IncludeNode node = nodes.at(i);
        if(node.file >= count || (node.parent != no_node && node.parent >= nodes.size())) {
            LOG_DEBUG("Rejecting TU index: include node past the path or node table");
            return {};
        }
    }
    auto symbols = SymbolColumns::of(root);
    auto symbol_count = symbols.hashes.size();
    if(symbols.name_ends.size() != symbol_count || symbols.args_ends.size() != symbol_count ||
       symbols.signature_ends.size() != symbol_count || symbols.parents.size() != symbol_count ||
       symbols.kinds.size() != symbol_count || symbols.scopes.size() != symbol_count ||
       symbols.flags.size() != symbol_count || symbols.token_kinds.size() != symbol_count ||
       symbols.token_modifiers.size() != symbol_count || !all_token_kinds(symbols.token_kinds) ||
       symbols.files.size() != symbol_count || symbols.reference_ends.size() != symbol_count ||
       symbols.instantiation_ends.size() != symbol_count ||
       !monotone_ends(symbols.name_ends, symbols.names.size()) ||
       !monotone_ends(symbols.args_ends, symbols.args.size()) ||
       !monotone_ends(symbols.signature_ends, symbols.signatures.size()) ||
       !monotone_ends(symbols.reference_ends, symbols.references.size()) ||
       !monotone_ends(symbols.instantiation_ends, symbols.instantiations.size())) {
        LOG_DEBUG("Rejecting TU index: malformed symbol columns");
        return {};
    }
    // Lookups binary-search the hashes. Hashes and parents become DenseMap
    // and DenseSet keys in every consumer (the project table, a query's
    // parent walk), and the two sentinel values corrupt or assert in those
    // containers; the builder never emits them, so a TU index carrying
    // one is corrupt. Merged reference files would persist behind versions
    // that match the disk, so a path id past the table must reject the
    // whole TU index rather than be dropped.
    for(std::size_t i = 0; i < symbol_count; i += 1) {
        if((i != 0 && symbols.hashes[i] <= symbols.hashes[i - 1]) ||
           reserved_key(symbols.hashes[i]) || reserved_key(symbols.parents[i])) {
            LOG_DEBUG("Rejecting TU index: unsorted or reserved symbol hash, or reserved parent");
            return {};
        }
        if(symbols.files[i] != no_file && symbols.files[i] >= count) {
            LOG_DEBUG("Rejecting TU index: symbol file past the path table");
            return {};
        }
    }
    auto past_paths = [&](std::uint32_t file) {
        return file >= count;
    };
    if(llvm::any_of(symbols.references, past_paths) ||
       llvm::any_of(symbols.instantiations, past_paths)) {
        LOG_DEBUG("Rejecting TU index: reference file past the path table");
        return {};
    }
    // section_of binary-searches the section table by path id and shard_of
    // trusts the result, so the ids must ascend strictly — a repeated or
    // out-of-order id would attribute one file's index to another.
    auto sections = root[&TUIndexBlob::sections];
    std::uint32_t previous_path_id = 0;
    for(std::size_t i = 0; i < sections.size(); i += 1) {
        auto path_id = sections.at(i)[&FileSection::path_id];
        if(path_id >= count || (i != 0 && path_id <= previous_path_id)) {
            LOG_DEBUG("Rejecting TU index: section path ids are not strictly ascending");
            return {};
        }
        previous_path_id = path_id;
    }
    // The merge maps skipped keys through their file's stored keys by
    // binary search.
    auto instantiations = root[&TUIndexBlob::instantiations];
    for(std::size_t i = 0; i < instantiations.size(); i += 1) {
        auto section = instantiations.at(i);
        auto path_id = section[&InstantiationSection::path_id];
        if(path_id >= count || (i != 0 && path_id <= previous_path_id) ||
           !std::ranges::is_sorted(to_array_ref(section[&InstantiationSection::skipped]),
                                   std::less_equal{})) {
            LOG_DEBUG("Rejecting TU index: instantiation sections are not strictly ascending");
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
    return loaded() ? blob_root(data)[&TUIndexBlob::built_at] : 0;
}

std::uint32_t TUIndex::path_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::paths].size()) : 0;
}

llvm::StringRef TUIndex::path(std::uint32_t id) const {
    return to_ref(blob_root(data)[&TUIndexBlob::paths].at(id));
}

std::uint32_t TUIndex::absent_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::absent].size()) : 0;
}

llvm::StringRef TUIndex::absent(std::uint32_t i) const {
    return to_ref(blob_root(data)[&TUIndexBlob::absent].at(i));
}

std::uint64_t TUIndex::path_hash(std::uint32_t id) const {
    // The hash column may be shorter than the path table on a foreign
    // blob; an absent hash reads as 0, "unavailable".
    auto hashes = blob_root(data)[&TUIndexBlob::path_hashes];
    return id < hashes.size() ? hashes.at(id) : 0;
}

std::uint32_t TUIndex::node_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::nodes].size()) : 0;
}

IncludeNode TUIndex::node(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::nodes].at(i);
}

std::uint32_t TUIndex::section_count() const {
    return loaded() ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::sections].size())
                    : 0;
}

std::uint32_t TUIndex::section_path(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::sections].at(i)[&FileSection::path_id];
}

std::uint64_t TUIndex::section_hash(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::sections].at(i)[&FileSection::hash];
}

llvm::StringRef TUIndex::section_blob(std::uint32_t i) const {
    auto blob = to_array_ref(blob_root(data)[&TUIndexBlob::sections].at(i)[&FileSection::blob]);
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

std::optional<std::uint32_t> TUIndex::section_of(std::uint32_t path_id) const {
    auto sections = std::views::iota(std::uint32_t(0), section_count());
    auto it = std::ranges::lower_bound(sections, path_id, {}, [&](std::uint32_t i) {
        return section_path(i);
    });
    if(it == sections.end() || section_path(*it) != path_id) {
        return std::nullopt;
    }
    return *it;
}

const Shard& TUIndex::shard_of(std::uint32_t path_id) const {
    const static Shard missing;
    auto section = section_of(path_id);
    return section ? section_shard(*section) : missing;
}

const Shard& TUIndex::section_shard(std::uint32_t section) const {
    if(shards.empty()) {
        shards.resize(section_count());
    }
    auto& slot = shards[section];
    if(!slot.loaded()) {
        slot = Shard::from_bytes(section_blob(section));
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
        // only the byte hash catches those, so a persisted TU index must
        // fail here and rebuild instead of serving corrupted entries.
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

std::uint32_t TUIndex::instantiation_count() const {
    return loaded()
               ? static_cast<std::uint32_t>(blob_root(data)[&TUIndexBlob::instantiations].size())
               : 0;
}

std::uint32_t TUIndex::instantiation_path(std::uint32_t i) const {
    return blob_root(data)[&TUIndexBlob::instantiations].at(i)[&InstantiationSection::path_id];
}

llvm::StringRef TUIndex::instantiation_blob(std::uint32_t i) const {
    auto blob = to_array_ref(
        blob_root(data)[&TUIndexBlob::instantiations].at(i)[&InstantiationSection::blob]);
    return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
}

llvm::ArrayRef<std::uint64_t> TUIndex::skipped_instantiations(std::uint32_t i) const {
    return to_array_ref(
        blob_root(data)[&TUIndexBlob::instantiations].at(i)[&InstantiationSection::skipped]);
}

const InstantiationShard& TUIndex::instantiation_shard(std::uint32_t i) const {
    if(instantiation_shards.empty()) {
        instantiation_shards.resize(instantiation_count());
    }
    auto& slot = instantiation_shards[i];
    if(!slot.loaded() && !instantiation_blob(i).empty()) {
        slot = InstantiationShard::from_bytes(instantiation_blob(i));
    }
    return slot;
}

std::optional<std::uint32_t> TUIndex::instantiation_section_of(std::uint32_t path_id) const {
    auto sections = std::views::iota(std::uint32_t(0), instantiation_count());
    auto it = std::ranges::lower_bound(sections, path_id, {}, [&](std::uint32_t i) {
        return instantiation_path(i);
    });
    if(it == sections.end() || instantiation_path(*it) != path_id) {
        return std::nullopt;
    }
    return *it;
}

void TUIndex::iterate_instantiation_files(
    llvm::function_ref<bool(SymbolHash, llvm::ArrayRef<std::uint32_t>)> callback) const {
    if(!loaded()) {
        return;
    }
    auto symbols = SymbolColumns::of(blob_root(data));
    for(std::uint32_t entry = 0; entry < symbols.hashes.size(); entry += 1) {
        auto files = symbols.instantiations_of(entry);
        if(!files.empty() && !callback(symbols.hashes[entry], files)) {
            return;
        }
    }
}

void TUIndex::iterate_symbols(
    llvm::function_ref<bool(SymbolHash, const SymbolIdentity&, llvm::ArrayRef<std::uint32_t>)>
        callback) const {
    if(!loaded()) {
        return;
    }
    auto symbols = SymbolColumns::of(blob_root(data));
    for(std::uint32_t entry = 0; entry < symbols.hashes.size(); entry += 1) {
        if(!callback(symbols.hashes[entry],
                     symbols.identity(entry),
                     symbols.references_of(entry))) {
            return;
        }
    }
}

std::optional<SymbolIdentity> TUIndex::find_symbol(SymbolHash hash) const {
    if(!loaded()) {
        return std::nullopt;
    }
    auto symbols = SymbolColumns::of(blob_root(data));
    if(auto entry = symbols.find(hash)) {
        return symbols.identity(*entry);
    }
    for(std::uint32_t section = 0; section < section_count(); section += 1) {
        if(auto identity = section_shard(section).find_symbol(hash)) {
            return identity;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<PathFanout>> TUIndex::local_fanout(const llvm::BitVector& held) const {
    std::vector<PathFanout> result;
    bool valid = true;
    iterate_symbols(
        [&](SymbolHash hash, const SymbolIdentity& identity, llvm::ArrayRef<std::uint32_t> files) {
            if(identity.scope == SymbolScope::External || files.size() < 2) {
                return true;
            }
            if(llvm::any_of(files, [&](std::uint32_t path_id) { return !held.test(path_id); })) {
                valid = false;
                return false;
            }
            result.push_back({
                .symbol = hash,
                .paths = {files.begin(), files.end()}
            });
            return true;
        });
    if(!valid) {
        return std::nullopt;
    }
    return result;
}

bool TUIndex::matches_prefix(llvm::StringRef text) const {
    if(!loaded()) {
        return false;
    }
    auto root = blob_root(data);
    auto size = root[&TUIndexBlob::preamble_size];
    return text.size() >= size &&
           llvm::xxh3_64bits(text.take_front(size)) == root[&TUIndexBlob::preamble_hash];
}

std::vector<DocumentLink> TUIndex::links() const {
    if(!loaded()) {
        return {};
    }
    auto entries = blob_root(data)[&TUIndexBlob::links];

    std::vector<DocumentLink> links;
    links.reserve(entries.size());
    for(std::size_t i = 0; i < entries.size(); i += 1) {
        auto entry = entries[i];
        links.push_back(DocumentLink{
            .range = entry[&DocumentLink::range],
            .target = std::string(entry[&DocumentLink::target]),
        });
    }
    return links;
}

llvm::ArrayRef<std::uint32_t> TUIndex::inactive_regions() const {
    if(!loaded()) {
        return {};
    }
    return to_array_ref(blob_root(data)[&TUIndexBlob::inactive_regions]);
}

llvm::ArrayRef<std::uint8_t> TUIndex::open_conditionals() const {
    if(!loaded()) {
        return {};
    }
    return to_array_ref(blob_root(data)[&TUIndexBlob::open_conditionals]);
}

llvm::StringRef TUIndex::preamble_diagnostics() const {
    if(!loaded()) {
        return {};
    }
    return to_ref(blob_root(data)[&TUIndexBlob::diagnostics]);
}

}  // namespace clice::index
