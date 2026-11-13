module clice;

import :index.indexing.builder;
import :index.symbol;
import :semantic.decls;
import :semantic.identity;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.template_resolver;
import :semantic.types;

namespace clice::index {

namespace {

/// The type of a value; of a function, the type it returns.
clang::QualType value_type(const clang::ValueDecl* decl) {
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getReturnType();
    }
    return decl->getType();
}

/// The declaration go-to-type-definition reaches from a value or a
/// function: the one its value_type names once pointers, references and
/// arrays are stripped, and inside a template once the resolver, if any,
/// resolved it.
const clang::NamedDecl* type_definition_of(const clang::ValueDecl* decl,
                                           types::TemplateResolver* resolver) {
    auto type = value_type(decl);
    return types::decl_of(types::unwrap(resolver ? resolver->resolve(type) : type), resolver);
}

/// The name a call is written through, where call hierarchy points at it:
/// `f` of `ns::f(x)`, `operator==` of `a.operator==(b)`, the operator token
/// of `a == b`, the pointer of `(*fp)(x)` or `(obj.*pmf)(x)`.
clang::SourceRange callee_name(const clang::CallExpr* call, const clang::ASTContext& context) {
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
        return written_name(DRE->getNameInfo(), context);
    }
    if(auto* ME = llvm::dyn_cast<clang::MemberExpr>(callee)) {
        return written_name(ME->getMemberNameInfo(), context);
    }
    if(auto* OE = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        return written_name(OE->getNameInfo(), context);
    }
    if(auto* DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        return written_name(DSDRE->getNameInfo(), context);
    }
    if(auto* DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        return written_name(DSME->getMemberNameInfo(), context);
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

}  // namespace

void Builder::relate(clang::FileID fid,
                     const clang::NamedDecl* decl,
                     Relation relation,
                     const clang::NamedDecl* target) {
    auto* index = file_index(fid);
    if(!index) {
        return;
    }
    if(target) {
        relation.target = ensure_symbol(decls::normalize(target));
    }
    index->relations[ensure_symbol(decls::normalize(decl))].emplace_back(relation);
}

void Builder::add_self_relation(const clang::NamedDecl* decl,
                                RelationKind kind,
                                clang::SourceRange name) {
    auto [fid, range] = *unit.written_range(name);
    Relation relation{.kind = kind, .range = range, .target = 0};

    // An instance variable a property synthesizes spans no declaration
    // of its own, an @synthesize names it at most.
    const auto* ivar = llvm::dyn_cast<clang::ObjCIvarDecl>(decl);
    if(kind.declares() && !(ivar && ivar->getSynthesize())) {
        auto source_range = decls::written_range(decl);
        if(source_range.isValid()) {
            auto [def_fid, extent] = unit.produced_range(source_range);
            // A declaration can begin in another file, e.g. when a
            // header-defined macro spells its leading tokens. Such a
            // range is meaningless in this file's coordinates; leave
            // the definition range empty instead of storing it.
            if(fid == def_fid) {
                relation.set_extent(extent);
            }
        }
    }
    relate(fid, decl, relation);
}

void Builder::add_instantiation(const clang::ExplicitInstantiationDecl& directive) {
    auto [fid, range] = *unit.written_range(unit.seen_location(directive.getNameLoc()));
    Relation relation{.kind = RelationKind::Instantiation, .range = range, .target = 0};
    if(auto [extent_fid, extent] = unit.produced_range(directive.getSourceRange());
       extent_fid == fid) {
        relation.set_extent(extent);
    }
    relate(fid, directive.getSpecialization(), relation);
}

void Builder::add_pair_relation(const clang::NamedDecl* decl,
                                RelationKind kind,
                                const clang::NamedDecl* target,
                                clang::SourceRange anchor) {
    // The anchor only routes the relation to a file's index; symbol pairs
    // carry no range of their own.
    relate(unit.produced_at(anchor.getBegin()).file, decl, {.kind = kind}, target);
}

void Builder::add_type_definitions(const clang::ValueDecl* decl) {
    auto* target = type_definition_of(decl, place_resolver);
    if(!target) {
        return;
    }
    add_pair_relation(decl, RelationKind::TypeDefinition, target, decl->getLocation());
    if(!place_resolver) {
        return;
    }
    auto type = types::unwrap(place_resolver->resolve(value_type(decl)));
    if(!llvm::isa<clang::CXXRecordDecl>(target) || type->isDependentType()) {
        return;
    }
    if(auto* pointed = types::decl_of(types::unwrap(place_resolver->arrow_pointee(type)))) {
        add_pair_relation(decl, RelationKind::TypeDefinition, pointed, decl->getLocation());
    }
}

void Builder::add_specialized_type(const clang::NamedDecl* decl, clang::SourceRange name) {
    if(!llvm::isa<clang::FieldDecl, clang::VarDecl, clang::FunctionDecl>(decl)) {
        return;
    }
    auto* pattern = decls::normalize(decl);
    if(pattern == decl->getCanonicalDecl()) {
        return;
    }
    auto* target = type_definition_of(llvm::cast<clang::ValueDecl>(decl), place_resolver);
    if(!target) {
        return;
    }
    auto* declared = type_definition_of(llvm::cast<clang::ValueDecl>(pattern), place_resolver);
    auto& entities = unit.get<EntityTable>();
    if(declared &&
       entities.entity(decls::normalize(declared)) == entities.entity(decls::normalize(target))) {
        return;
    }
    auto [fid, range] = *unit.written_range(name);
    relate(fid, decl, {.kind = RelationKind::TypeDefinition, .range = range}, target);
}

void Builder::add_call_relation(const clang::NamedDecl* decl,
                                RelationKind kind,
                                const clang::NamedDecl* target,
                                clang::SourceRange range) {
    auto [fid, relation_range] = unit.produced_range(range);
    relate(fid, decl, {.kind = kind, .range = relation_range}, target);
}

const clang::NamedDecl* Builder::enclosing_function(const Semantics& semantics,
                                                    std::uint32_t index) {
    llvm::SmallVector<std::uint32_t> path;
    const clang::NamedDecl* result = nullptr;

    for(auto p = semantics.node(index).parent; p != Semantics::invalid;
        p = semantics.node(p).parent) {
        if(p < enclosing_memo.size() && enclosing_memo[p]) {
            result = *enclosing_memo[p];
            break;
        }
        const auto* decl = semantics.node(p).node.get<clang::NamedDecl>();
        if(llvm::isa_and_present<clang::FunctionDecl, clang::ObjCMethodDecl>(decl)) {
            result = decl;
            break;
        }
        path.push_back(p);
    }

    for(auto p: path) {
        if(p >= enclosing_memo.size()) {
            enclosing_memo.resize(p + 1);
        }
        enclosing_memo[p] = result;
    }
    return result ? result : instantiated_function;
}

const clang::Decl* Builder::enclosing_top_level(const Semantics& semantics, std::uint32_t index) {
    auto at_file_scope = [](const clang::DeclContext* context) {
        return context->isFileContext() ||
               llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context);
    };

    llvm::SmallVector<std::uint32_t> path;
    const clang::Decl* result = nullptr;
    for(auto p = index; p != Semantics::invalid; p = semantics.node(p).parent) {
        if(p < top_level_memo.size() && top_level_memo[p]) {
            result = *top_level_memo[p];
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
        if(p >= top_level_memo.size()) {
            top_level_memo.resize(p + 1);
        }
        top_level_memo[p] = result;
    }
    return result;
}

clang::SourceLocation Builder::pasted_into(const clang::Decl* host,
                                           clang::SourceLocation location) {
    auto host_file = unit.produced_at(host->getLocation()).file;
    if(unit.produced_at(location).file == host_file) {
        return {};
    }
    while(location.isValid() && unit.produced_at(location).file != host_file) {
        location = unit.include_location(unit.produced_at(location).file);
    }
    // Reaching the file is not enough: a redeclaration shares the default
    // arguments its header's declaration wrote.
    auto extent = unit.produced_bounds(host->getSourceRange());
    if(location.isInvalid() || unit.before(location, extent.getBegin()) ||
       unit.before(extent.getEnd(), location)) {
        return {};
    }
    return location;
}

clang::SourceRange Builder::in_caller_file(const clang::NamedDecl* caller,
                                           clang::SourceRange range) {
    auto include = pasted_into(caller, unit.produced_bounds(range).getBegin());
    return include.isValid() ? clang::SourceRange(include) : range;
}

void Builder::add_pasted_relation(const clang::NamedDecl* decl,
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
    relate(fid, decl, {.kind = RelationKind::Pasted, .range = range});
}

void Builder::project_relations(const Semantics& semantics, std::uint32_t index) {
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
        auto range = callee_name(CE, context);
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
        // for it, as its candidate and guess references do.
        if(place_resolver) {
            for(auto* candidate: place_resolver->callees(CE)) {
                call(candidate->getUnderlyingDecl(), range);
            }
        }
        return;
    }

    // A property's accessor clang declares is one symbol with the method
    // an @implementation, maybe in another unit, writes for it: a call
    // to one nothing writes finds no callee. A message naming one refers
    // to the property in the semantic map, and to the accessor here,
    // where that method, and the other methods of its selector, find it.
    if(auto* OME = node.get<clang::ObjCMessageExpr>()) {
        const auto* method = decls::objc_message_target(*OME);
        if(!method) {
            return;
        }
        auto name = decls::objc_message_name(*OME);
        call(method, name);
        if(method->isImplicit()) {
            add_self_relation(method,
                              decls::objc_untyped(*OME) ? RelationKind::Candidate
                                                        : RelationKind::Reference,
                              name);
        }
        // `[super draw]` runs the superclass's method, never an override.
        if(OME->getReceiverKind() == clang::ObjCMessageExpr::SuperInstance ||
           OME->getReceiverKind() == clang::ObjCMessageExpr::SuperClass) {
            add_self_relation(method, RelationKind::Direct, name);
        }
        return;
    }
    // `shape.count` calls its accessors, `+=` both, and refers to them
    // as a message does; `super.count` calls the superclass's, never an
    // override.
    if(auto* OPRE = node.get<clang::ObjCPropertyRefExpr>()) {
        auto type = OPRE->getReceiverType(context);
        if(const auto* pointer = type->getAsObjCInterfacePointerType()) {
            type = pointer->getPointeeType();
        }
        const auto* object = type->getAsObjCInterfaceType();
        const auto* receiver = object ? object->getInterface() : nullptr;
        auto call_accessor = [&](const clang::ObjCMethodDecl* method) {
            if(!method) {
                return;
            }
            const auto* called =
                decls::objc_dispatched(receiver, *method, method->isInstanceMethod());
            call(called, OPRE->getLocation());
            add_self_relation(called, RelationKind::Reference, OPRE->getLocation());
            if(OPRE->isSuperReceiver()) {
                add_self_relation(called, RelationKind::Direct, OPRE->getLocation());
            }
        };
        const auto* property = OPRE->isExplicitProperty() ? OPRE->getExplicitProperty() : nullptr;
        if(OPRE->isMessagingGetter()) {
            call_accessor(property ? property->getGetterMethodDecl()
                                   : OPRE->getImplicitPropertyGetter());
        }
        if(OPRE->isMessagingSetter()) {
            call_accessor(property ? property->getSetterMethodDecl()
                                   : OPRE->getImplicitPropertySetter());
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
        if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(D);
           VTSD && clang::isTemplateInstantiation(VTSD->getSpecializationKind())) {
            return;
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

    // An Objective-C class's bases are its superclass and the protocols
    // it or a category of it adopts; a protocol's are the protocols it
    // inherits.
    auto add_base = [&](const clang::NamedDecl* derived,
                        const clang::NamedDecl* base,
                        clang::SourceLocation location) {
        if(derived && base) {
            add_pair_relation(derived, RelationKind::Base, base, location);
            add_pair_relation(base, RelationKind::Derived, derived, location);
        }
    };
    auto add_protocols = [&](const clang::NamedDecl* derived, auto&& protocols, auto&& locations) {
        for(auto [protocol, location]: views::zip(protocols, locations)) {
            add_base(derived, protocol, location);
        }
    };
    if(auto* ID = llvm::dyn_cast<clang::ObjCInterfaceDecl>(D)) {
        if(ID->isThisDeclarationADefinition()) {
            add_base(ID, ID->getSuperClass(), ID->getSuperClassLoc());
            add_protocols(ID, ID->protocols(), ID->protocol_locs());
        }
        return;
    }
    if(auto* CD = llvm::dyn_cast<clang::ObjCCategoryDecl>(D)) {
        add_protocols(CD->getClassInterface(), CD->protocols(), CD->protocol_locs());
        return;
    }
    if(auto* PD = llvm::dyn_cast<clang::ObjCProtocolDecl>(D)) {
        if(PD->isThisDeclarationADefinition()) {
            add_protocols(PD, PD->protocols(), PD->protocol_locs());
        }
        return;
    }
    // `@implementation Quill` defines the class an alias names, and
    // references the alias: a rename of the alias renames it. The
    // semantic map keeps the class alone there, as its token shows it.
    if(auto* IMD = llvm::dyn_cast<clang::ObjCImplementationDecl>(D)) {
        if(const auto* alias = llvm::dyn_cast<clang::ObjCCompatibleAliasDecl>(
               decls::objc_class_name(IMD->getClassInterface(), IMD->getLocation()))) {
            add_self_relation(alias, RelationKind::Reference, IMD->getLocation());
        }
        return;
    }
    // A method overrides its superclasses' methods of its selector and
    // implements the protocols' its class adopts.
    if(auto* MD = llvm::dyn_cast<clang::ObjCMethodDecl>(D)) {
        llvm::SmallVector<const clang::ObjCMethodDecl*, 4> overridden;
        MD->getOverriddenMethods(overridden);
        if(const auto* nearest = decls::objc_overridden(*MD);
           nearest && ranges::none_of(overridden, [&](const clang::ObjCMethodDecl* base) {
               return decls::normalize(base) == decls::normalize(nearest);
           })) {
            overridden.push_back(nearest);
        }
        for(const auto* base: overridden) {
            add_pair_relation(MD, RelationKind::Interface, base, MD->getLocation());
            add_pair_relation(base, RelationKind::Implementation, MD, MD->getLocation());
        }
        return;
    }

    // Base/derived edges, recorded at the defining declaration.
    if(auto* TD = llvm::dyn_cast<clang::TagDecl>(D)) {
        if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(TD);
           CTSD && CTSD->getSpecializationKind() != clang::TSK_ExplicitSpecialization) {
            return;
        }

        if(auto* CRD = llvm::dyn_cast<clang::CXXRecordDecl>(TD)) {
            if(auto* def = CRD->getDefinition()) {
                for(auto& base: CRD->bases()) {
                    // Through any alias the base is written with: the
                    // hierarchy relates classes.
                    auto target = types::decl_of(base.getType().getCanonicalType());
                    /// A base that is a template parameter (`struct D : T`)
                    /// names no class until instantiation: project_bases
                    /// records it per instantiation.
                    if(!target ||
                       llvm::isa<clang::TemplateTypeParmDecl, clang::TemplateTemplateParmDecl>(
                           target)) {
                        continue;
                    }
                    add_pair_relation(def, RelationKind::Base, target, base.getSourceRange());
                    add_pair_relation(target, RelationKind::Derived, def, base.getSourceRange());
                }
            }
        }
        return;
    }

    if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(D)) {
        if(clang::isTemplateInstantiation(FD->getTemplateSpecializationKind())) {
            return;
        }

        add_type_definitions(FD);

        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(FD)) {
            for(auto* base: method->overridden_methods()) {
                add_pair_relation(method, RelationKind::Interface, base, FD->getLocation());
                add_pair_relation(base, RelationKind::Implementation, method, FD->getLocation());
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

        if(auto* guide = llvm::dyn_cast<clang::CXXDeductionGuideDecl>(FD)) {
            add_pair_relation(guide->getDeducedTemplate(),
                              RelationKind::Guide,
                              guide,
                              FD->getLocation());
        }
        return;
    }
}

void Builder::project_placeholder(const Semantics& semantics, std::uint32_t index) {
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
        } else if(auto subst_loc = loc->getAs<clang::SubstTemplateTypeParmTypeLoc>()) {
            const auto* param = decls::replaced_parameter(*subst_loc.getTypePtr());
            if(param && param->isImplicit()) {
                keyword = subst_loc.getNameLoc();
                substituted = subst_loc.getTypePtr()->getReplacementType();
            }
        }
    } else if(const auto* function = entry.node.get<clang::FunctionDecl>()) {
        if(const auto* proto = function->getType()->getAs<clang::FunctionProtoType>();
           proto && proto->hasTrailingReturn()) {
            keyword = function->getTypeSpecStartLoc();
        }
    }
    if(keyword.isInvalid() || (entry.flags.in_instantiation && !instantiation_entries)) {
        return;
    }
    auto name = unit.seen_name(keyword);
    if(!name.written) {
        return;
    }
    // An init-capture's `auto` is implicit, at the captured name.
    auto spelling = unit.token_spelling(name.range.getBegin());
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
        if(!var || !var->getInit() || !place_resolver) {
            return;
        }
        type = place_resolver->deduce(var);
        if(type.isNull()) {
            return;
        }
    }

    for(const auto* target:
        types::decls_of(types::unwrap(readable.readable(type, decl->getDeclContext())),
                        place_resolver)) {
        // A closure type is declared by no name to reach.
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(target);
           record && record->isLambda()) {
            continue;
        }
        add_occurrence(target, name.range);
        add_self_relation(target, RelationKind::Deduced, name.range);
    }
}

void Builder::add_deduced_type(const clang::ValueDecl* decl) {
    if(!llvm::isa<clang::VarDecl>(decl)) {
        return;
    }
    const auto* pattern = llvm::cast<clang::ValueDecl>(in_pattern(decl));
    auto declared = pattern->getType();
    if(const auto* expansion = declared->getAs<clang::PackExpansionType>()) {
        declared = expansion->getPattern();
    }
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
    auto add = [&](const clang::NamedDecl* type_decl) {
        relate(fid, decl, {.kind = RelationKind::TypeDefinition, .range = range}, type_decl);
    };
    add(target);
    // A smart pointer leads on to its pointee, as add_type_definitions.
    if(llvm::isa<clang::CXXRecordDecl>(target) && place_resolver) {
        if(auto* pointed = types::decl_of(types::unwrap(place_resolver->arrow_pointee(type)))) {
            add(pointed);
        }
    }
}

}  // namespace clice::index
