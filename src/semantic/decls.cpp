/// Parts of this file (only_instantiation) are ported from clangd's AST.cpp
/// (llvmorg-21.1.8), and resolve_forwarding_params follows clangd's
/// resolveForwardingParameters;
/// LLVM project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

module clice;

import :semantic.decls;
import :semantic.template_resolver;
import :support.text;
import :syntax.lexer;

namespace clice::decls {

bool is_templated(const clang::Decl* decl) {
    assert(decl);
    if(decl->getDescribedTemplate()) {
        return true;
    }

    if(llvm::isa<clang::TemplateDecl,
                 clang::ClassTemplatePartialSpecializationDecl,
                 clang::VarTemplatePartialSpecializationDecl>(decl)) {
        return true;
    }

    return false;
}

auto own_template_parameters(const clang::Decl* decl) -> const clang::TemplateParameterList* {
    if(const auto* templated = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
        return templated->getTemplateParameters();
    }
    if(const auto* partial = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
        return partial->getTemplateParameters();
    }
    if(const auto* partial = llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
        return partial->getTemplateParameters();
    }
    return nullptr;
}

namespace {

/// The parameter list a substitution's associated declaration stands for,
/// as clang's getReplacedTemplateParameter reads it.
auto replaced_parameters(const clang::Decl* associated) -> const clang::TemplateParameterList* {
    if(const auto* list = own_template_parameters(associated)) {
        return list;
    }
    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(associated)) {
        auto from = spec->getSpecializedTemplateOrPartial();
        if(const auto* partial = from.dyn_cast<clang::ClassTemplatePartialSpecializationDecl*>()) {
            return partial->getTemplateParameters();
        }
        return llvm::cast<clang::ClassTemplateDecl*>(from)->getTemplateParameters();
    }
    if(const auto* spec = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(associated)) {
        auto from = spec->getSpecializedTemplateOrPartial();
        if(const auto* partial = from.dyn_cast<clang::VarTemplatePartialSpecializationDecl*>()) {
            return partial->getTemplateParameters();
        }
        return llvm::cast<clang::VarTemplateDecl*>(from)->getTemplateParameters();
    }
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(associated)) {
        const auto* described = record->getDescribedClassTemplate();
        return described ? described->getTemplateParameters() : nullptr;
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(associated)) {
        const auto* info = function->getTemplateSpecializationInfo();
        return info ? info->getTemplate()->getTemplateParameters() : nullptr;
    }
    return nullptr;
}

template <typename Param>
auto replaced_parameter(const clang::Decl* associated, unsigned index) -> const Param* {
    const auto* list = replaced_parameters(associated);
    return list && index < list->size() ? llvm::dyn_cast<Param>(list->getParam(index)) : nullptr;
}

}  // namespace

auto replaced_parameter(const clang::SubstTemplateTypeParmType& type)
    -> const clang::TemplateTypeParmDecl* {
    /// A deduced placeholder's substitution is associated with the
    /// invented parameter itself.
    if(const auto* param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(type.getAssociatedDecl())) {
        return param;
    }
    return replaced_parameter<clang::TemplateTypeParmDecl>(type.getAssociatedDecl(),
                                                           type.getIndex());
}

auto replaced_parameter(const clang::SubstNonTypeTemplateParmPackExpr& expr)
    -> const clang::NonTypeTemplateParmDecl* {
    return replaced_parameter<clang::NonTypeTemplateParmDecl>(expr.getAssociatedDecl(),
                                                              expr.getIndex());
}

auto outer_template_parameters(const clang::Decl* decl)
    -> llvm::ArrayRef<clang::TemplateParameterList*> {
    if(const auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(decl)) {
        return declarator->getTemplateParameterLists();
    }
    if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
        return tag->getTemplateParameterLists();
    }
    return {};
}

namespace {

/// Where the attribute specifier holding `attr`, the first attribute of
/// it, is written: at the `[[` or the `__attribute__((` its range leaves
/// out (`[[using gnu: hot]]` starts at the namespace), a macro spelling it
/// at the macro's name.
auto attribute_begin(const clang::Attr& attr,
                     const clang::SourceManager& sources,
                     const clang::LangOptions& lang_opts) -> clang::SourceLocation {
    auto begin = attr.getRange().getBegin();
    if(begin.isMacroID() || attr.isAlignas()) {
        return sources.getExpansionLoc(begin);
    }
    auto previous = [&](clang::SourceLocation at) {
        return clang::Lexer::findPreviousToken(at, sources, lang_opts, false);
    };
    if(attr.isGNUAttribute()) {
        auto at = begin;
        for(int parens = 0; parens < 2; parens += 1) {
            auto paren = previous(at);
            if(!paren || !paren->is(clang::tok::l_paren)) {
                return begin;
            }
            at = paren->getLocation();
        }
        // A raw token: the keyword is an identifier still.
        auto keyword = previous(at);
        return keyword && keyword->is(clang::tok::raw_identifier) &&
                       keyword->getRawIdentifier().starts_with("__attribute")
                   ? keyword->getLocation()
                   : begin;
    }
    if(!attr.isStandardAttributeSyntax()) {
        return begin;
    }
    auto at = begin;
    for(int tokens = 0; tokens < 5; tokens += 1) {
        auto token = previous(at);
        if(!token) {
            break;
        }
        at = token->getLocation();
        if(token->is(clang::tok::l_square)) {
            auto outer = previous(at);
            return outer && outer->is(clang::tok::l_square) ? outer->getLocation() : begin;
        }
    }
    return begin;
}

}  // namespace

auto written_range(const clang::Decl* decl) -> clang::SourceRange {
    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
       named && is_instantiation(named)) {
        if(const auto* pattern = instantiated_from(named); pattern && pattern != named) {
            return written_range(pattern);
        }
    }
    auto& sources = decl->getASTContext().getSourceManager();
    auto before = [&](clang::SourceLocation lhs, clang::SourceLocation rhs) {
        return rhs.isInvalid() ||
               (lhs.isValid() && sources.isBeforeInTranslationUnit(sources.getExpansionLoc(lhs),
                                                                   sources.getExpansionLoc(rhs)));
    };
    auto range = decl->getSourceRange();
    // An out-of-line member template of a class template starts at the
    // class's `template` header, which its own leaves out.
    if(const auto* described = decl->getDescribedTemplate();
       described && before(described->getBeginLoc(), range.getBegin())) {
        range.setBegin(described->getBeginLoc());
    }
    // The first attribute written before the declaration, or where clang
    // starts it: an Objective-C container starts at its attribute's name.
    const clang::Attr* leading = nullptr;
    for(const auto* attr: decl->attrs()) {
        auto begin = attr->getRange().getBegin();
        if(attr->isImplicit() || attr->isInherited() || begin.isInvalid() ||
           before(range.getBegin(), begin) || applied_by_pragma(decl->getASTContext(), begin)) {
            continue;
        }
        if(!leading || before(begin, leading->getRange().getBegin())) {
            leading = attr;
        }
    }
    if(leading) {
        range.setBegin(attribute_begin(*leading, sources, decl->getASTContext().getLangOpts()));
    }
    // Clang leaves a bound it cannot locate unset, on valid code too:
    // `short __attribute__((vector_size(16)))`, `Ts...[0]`, the typeless
    // `for(x : v)`; an unclosed `namespace a {` runs to the end of its file.
    if(range.getBegin().isInvalid()) {
        range.setBegin(decl->getLocation());
    }
    if(range.getEnd().isInvalid()) {
        range.setEnd(llvm::isa<clang::NamespaceDecl>(decl)
                         ? sources.getLocForEndOfFile(sources.getFileID(range.getBegin()))
                         : decl->getLocation());
    }
    // An alias runs on to the class it names; clang ends it at the alias.
    if(const auto* alias = llvm::dyn_cast<clang::ObjCCompatibleAliasDecl>(decl)) {
        if(auto named = objc_alias_names(*alias).second; named.isValid()) {
            range.setEnd(named);
        }
    }
    // An @implementation's range stops at the `@` of its `@end`.
    if(llvm::isa<clang::ObjCImplDecl>(decl)) {
        const auto& options = decl->getASTContext().getLangOpts();
        clang::Token at;
        if(!clang::Lexer::getRawToken(range.getEnd(), at, sources, options) &&
           at.is(clang::tok::at)) {
            if(auto end = clang::Lexer::findNextToken(range.getEnd(), sources, options)) {
                range.setEnd(end->getLocation());
            }
        }
    }
    return range;
}

bool applied_by_pragma(const clang::ASTContext& context, clang::SourceLocation location) {
    auto& sources = context.getSourceManager();
    for(auto at = location; at.isMacroID();) {
        at = sources.getImmediateExpansionRange(at).getBegin();
        clang::Token token;
        if(!clang::Lexer::getRawToken(sources.getSpellingLoc(at),
                                      token,
                                      sources,
                                      context.getLangOpts()) &&
           token.is(clang::tok::raw_identifier) && token.getRawIdentifier() == "_Pragma") {
            return true;
        }
    }
    auto [file, offset] = sources.getDecomposedLoc(sources.getExpansionLoc(location));
    auto content = sources.getBufferData(file);
    Lexer lexer(content.substr(logical_line_begin(content, offset)), context.getLangOpts());
    return lexer.advance().is_directive_hash();
}

const clang::NamespaceDecl* outermost_namespace(const clang::Decl* decl) {
    const clang::NamespaceDecl* outermost = nullptr;
    for(const auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
            outermost = ns;
        }
    }
    return outermost;
}

bool is_exported(const clang::Decl* decl) {
    // A concept's or alias template's parameters sit in the enclosing
    // context, the `export` block included.
    if(decl->isTemplateParameter() ||
       !decl->getDeclContext()->getRedeclContext()->isFileContext()) {
        return false;
    }
    // Clang marks what a named module exports visible to importers — a
    // namespace too once it holds an exported declaration.
    return ranges::any_of(decl->redecls(), [](const clang::Decl* redecl) {
        auto* module = redecl->getOwningModule();
        return module && module->isNamedModule() &&
               redecl->getModuleOwnershipKind() ==
                   clang::Decl::ModuleOwnershipKind::VisibleWhenImported;
    });
}

namespace {

template <class T>
bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                     clang::TemplateSpecializationKind kind) {
    if(const auto* td = dyn_cast<T>(decl))
        return td->getTemplateSpecializationKind() == kind;
    return false;
}

inline bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                            clang::TemplateSpecializationKind kind) {
    return is_template_specialization_kind<clang::FunctionDecl>(decl, kind) ||
           is_template_specialization_kind<clang::CXXRecordDecl>(decl, kind) ||
           is_template_specialization_kind<clang::VarDecl>(decl, kind);
}

}  // namespace

bool is_implicit_instantiation(const clang::NamedDecl* decl) {
    assert(decl);
    return is_template_specialization_kind(decl, clang::TSK_ImplicitInstantiation);
}

bool in_standard_library(const clang::Decl* decl) {
    auto& context = decl->getASTContext();
    if(!context.getSourceManager().isInSystemHeader(decl->getLocation())) {
        return false;
    }
    for(const auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
        if(scope->isStdNamespace()) {
            return true;
        }
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope);
           ns && ns->getIdentifier() &&
           clang::isReservedInAllContexts(ns->getIdentifier()->isReserved(context.getLangOpts()))) {
            return true;
        }
    }
    return false;
}

bool is_instantiation(const clang::Decl* decl) {
    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
       named && is_implicit_instantiation(named)) {
        return true;
    }

    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        return !llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec) &&
               clang::isTemplateInstantiation(spec->getSpecializationKind());
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return clang::isTemplateInstantiation(function->getTemplateSpecializationKind());
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return clang::isTemplateInstantiation(var->getTemplateSpecializationKind());
    }
    /// A member class of a class template specialization, instantiated
    /// along with it or explicitly (`template struct Outer<int>::Inner;`).
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        return clang::isTemplateInstantiation(record->getTemplateSpecializationKind());
    }
    return false;
}

namespace {

const clang::CXXRecordDecl* getDeclContextForTemplateInstationPattern(const clang::Decl* D) {
    if(const auto* CTSD = dyn_cast<clang::ClassTemplateSpecializationDecl>(D->getDeclContext())) {
        return CTSD->getTemplateInstantiationPattern();
    }

    if(const auto* RD = dyn_cast<clang::CXXRecordDecl>(D->getDeclContext())) {
        return RD->getInstantiatedFromMemberClass();
    }

    return nullptr;
}

}  // namespace

auto function_pattern(const clang::FunctionDecl& function) -> const clang::FunctionDecl* {
    if(const auto* pattern = function.getTemplateInstantiationPattern()) {
        return pattern;
    }
    if(const auto* described = function.getDescribedFunctionTemplate()) {
        if(const auto* member = described->getInstantiatedFromMemberTemplate();
           member && !described->isMemberSpecialization()) {
            return member->getTemplatedDecl();
        }
    }
    return nullptr;
}

auto named_parameters(const clang::FunctionDecl& function, unsigned index)
    -> llvm::SmallVector<const clang::ParmVarDecl*, 2> {
    llvm::SmallVector<const clang::ParmVarDecl*, 2> named;
    auto add = [&](const clang::FunctionDecl& declaration) {
        for(const auto* redecl: declaration.redecls()) {
            // A C declaration without a prototype has no parameters.
            if(index < redecl->getNumParams() && redecl->getParamDecl(index)->getIdentifier()) {
                named.push_back(redecl->getParamDecl(index));
            }
        }
    };
    // clang renames an instantiation's parameters after the definition's
    // when it instantiates the body, which another compile may not have
    // done: the declarations it was instantiated from name them alone.
    if(const auto* pattern = function_pattern(function);
       pattern && pattern->getNumParams() == function.getNumParams()) {
        add(*pattern);
    } else {
        add(function);
    }
    // LIMITATION: an undocumented override renaming a parameter of the base
    // whose comment it inherits (`documentation.inherit`) finds no `@param`
    // for it: hover and signature help give the parameter no text, and the
    // method's card lists it under the base's name.
    return named;
}

auto named_parameters(const clang::ParmVarDecl& param)
    -> llvm::SmallVector<const clang::ParmVarDecl*, 2> {
    auto index = param.getFunctionScopeIndex();
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(param.getDeclContext());
       function && index < function->getNumParams() && function->getParamDecl(index) == &param) {
        return named_parameters(*function, index);
    }
    if(param.getIdentifier()) {
        return {&param};
    }
    return {};
}

bool std_cast_form(const clang::FunctionDecl* function) {
    switch(function->getBuiltinID()) {
        case clang::Builtin::BIaddressof:
        case clang::Builtin::BIas_const:
        case clang::Builtin::BIforward:
        case clang::Builtin::BIforward_like:
        case clang::Builtin::BImove:
        case clang::Builtin::BImove_if_noexcept: return true;
        default: break;
    }

    // Freestanding compiles (-ffreestanding) strip library-builtin IDs, so
    // match the same set by name. All of these are single-parameter
    // cast-like forms — the check must not take in e.g. the three-argument
    // std::move algorithm.
    auto* name = function->getIdentifier();
    if(name && function->getNumParams() == 1 && function->isInStdNamespace()) {
        return ranges::contains(std::array<llvm::StringRef, 6>{"addressof",
                                                               "as_const",
                                                               "forward",
                                                               "forward_like",
                                                               "move",
                                                               "move_if_noexcept"},
                                name->getName());
    }
    return false;
}

bool fixed_std_utility(const clang::NamedDecl* decl) {
    auto* function = decl->getAsFunction();
    if(!function) {
        return false;
    }
    if(std_cast_form(function)) {
        return true;
    }
    auto* name = function->getIdentifier();
    return name && name->isStr("declval") && function->isInStdNamespace();
}

auto instantiated_from(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    assert(decl);
    if(auto CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        auto kind = CTSD->getTemplateSpecializationKind();
        if(kind == clang::TSK_Undeclared && explicitly_specialized(CTSD)) {
            return CTSD;
        }
        if(kind == clang::TSK_Undeclared) {
            /// Instantiation is lazy: an undeclared specialization carries no
            /// pattern link yet. Select the pattern instantiation would use so
            /// the identity matches the instantiated case.
            return types::instantiation_pattern(*CTSD);
        } else if(kind == clang::TSK_ExplicitSpecialization) {
            /// If the decl is an full specialization, return itself.
            return CTSD;
        }

        return CTSD->getTemplateInstantiationPattern();
    }

    if(auto FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        /// If the decl is an full specialization, return itself.
        if(FD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return FD;
        }

        return FD->getTemplateInstantiationPattern();
    }

    if(auto VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
        if(VTSD->getSpecializationKind() == clang::TSK_Undeclared) {
            if(explicitly_specialized(VTSD)) {
                return VTSD;
            }
            return types::instantiation_pattern(*VTSD);
        }
    }

    if(auto VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
        /// If the decl is an full specialization, return itself.
        if(VD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return VD;
        }

        return VD->getTemplateInstantiationPattern();
    }

    if(auto CRD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        /// An explicitly specialized member (`template <> struct Outer<char>::Inner`)
        /// is its own entity, just like a full specialization.
        if(CRD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return CRD;
        }
        return CRD->getInstantiatedFromMemberClass();
    }

    /// For `FieldDecl` and `TypedefNameDecl`, clang will not store their instantiation information
    /// in the unit. So we need to look up the original decl manually.
    if(llvm::isa<clang::FieldDecl, clang::TypedefNameDecl>(decl)) {
        if(auto context = getDeclContextForTemplateInstationPattern(decl)) {
            for(auto member: context->lookup(decl->getDeclName())) {
                if(member->isImplicit()) {
                    continue;
                }

                if(member->getKind() == decl->getKind()) {
                    return member;
                }
            }
        }
    }

    if(auto ED = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        if(auto* info = ED->getMemberSpecializationInfo();
           info && info->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return ED;
        }
        return ED->getInstantiatedFromMemberEnum();
    }

    if(auto ECD = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        auto ED = llvm::cast<clang::EnumDecl>(ECD->getDeclContext());
        if(auto context = ED->getInstantiatedFromMemberEnum()) {
            for(auto member: context->lookup(ECD->getDeclName())) {
                return member;
            }
        }
    }

    return nullptr;
}

auto normalize(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    assert(decl);

    decl = llvm::cast<clang::NamedDecl>(objc_defined(decl)->getCanonicalDecl());

    if(auto ND = instantiated_from(llvm::cast<clang::NamedDecl>(decl))) {
        return llvm::cast<clang::NamedDecl>(ND->getCanonicalDecl());
    }

    return decl;
}

auto objc_defined(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    if(const auto* implementation = llvm::dyn_cast<clang::ObjCImplementationDecl>(decl)) {
        return implementation->getClassInterface();
    }
    if(const auto* implementation = llvm::dyn_cast<clang::ObjCCategoryImplDecl>(decl);
       implementation && implementation->getCategoryDecl()) {
        return implementation->getCategoryDecl();
    }
    return decl;
}

auto objc_class(const clang::DeclContext* context) -> const clang::ObjCInterfaceDecl* {
    if(const auto* objc_class = llvm::dyn_cast<clang::ObjCInterfaceDecl>(context)) {
        return objc_class;
    }
    if(const auto* implementation = llvm::dyn_cast<clang::ObjCImplDecl>(context)) {
        return implementation->getClassInterface();
    }
    if(const auto* category = llvm::dyn_cast<clang::ObjCCategoryDecl>(context)) {
        return category->getClassInterface();
    }
    return nullptr;
}

/// The method `selector` names in an Objective-C class itself: in its
/// @interface, its extensions and categories, or their @implementations,
/// which come first: they write the accessors clang declares for a property.
static auto own_method(const clang::ObjCInterfaceDecl& objc_class,
                       clang::Selector selector,
                       bool instance) -> const clang::ObjCMethodDecl* {
    auto in = [&](const clang::ObjCContainerDecl* container) -> const clang::ObjCMethodDecl* {
        return container ? container->getMethod(selector, instance) : nullptr;
    };
    if(const auto* method = in(objc_class.getImplementation())) {
        return method;
    }
    for(const auto* category: objc_class.visible_categories()) {
        if(const auto* method = in(category->getImplementation())) {
            return method;
        }
    }
    if(const auto* method = in(&objc_class)) {
        return method;
    }
    for(const auto* category: objc_class.visible_categories()) {
        if(const auto* method = in(category)) {
            return method;
        }
    }
    return nullptr;
}

auto objc_dispatched(const clang::ObjCInterfaceDecl* receiver,
                     const clang::ObjCMethodDecl& method,
                     bool instance) -> const clang::ObjCMethodDecl* {
    const auto* declaring = objc_class(method.getDeclContext());
    for(const auto* objc_class = receiver;
        objc_class && objc_class != declaring && objc_class->hasDefinition();
        objc_class = objc_class->getSuperClass()) {
        if(const auto* own = own_method(*objc_class, method.getSelector(), instance)) {
            return own;
        }
    }
    if(method.isImplicit() && declaring) {
        if(const auto* written =
               own_method(*declaring, method.getSelector(), method.isInstanceMethod());
           written && !written->isImplicit()) {
            return written;
        }
    }
    return &method;
}

auto objc_message_name(const clang::ObjCMessageExpr& message) -> clang::SourceLocation {
    auto selector = message.getSelector();
    for(unsigned piece = 0; piece < message.getNumSelectorLocs(); piece += 1) {
        if(!selector.getNameForSlot(piece).empty()) {
            return message.getSelectorLoc(piece);
        }
    }
    return message.getSelectorStartLoc();
}

auto objc_untyped(const clang::ObjCMessageExpr& message) -> bool {
    const auto* receiver = message.getReceiverKind() == clang::ObjCMessageExpr::Instance
                               ? message.getInstanceReceiver()
                               : nullptr;
    return receiver && !receiver->isObjCSelfExpr() &&
           (receiver->getType()->isObjCIdType() || receiver->getType()->isObjCClassType());
}

auto objc_message_target(const clang::ObjCMessageExpr& message) -> const clang::ObjCMethodDecl* {
    const auto* method = message.getMethodDecl();
    if(!method) {
        return nullptr;
    }
    const auto* receiver = message.getReceiverInterface();
    // `self` in a class method is a `Class`: the method's class's, or a
    // subclass's.
    if(const auto* self = message.getReceiverKind() == clang::ObjCMessageExpr::Instance
                              ? llvm::dyn_cast<clang::DeclRefExpr>(
                                    message.getInstanceReceiver()->IgnoreParenImpCasts())
                              : nullptr;
       !receiver && self && self->isObjCSelfExpr()) {
        receiver = objc_class(self->getDecl()->getDeclContext()->getParent());
    }
    return objc_dispatched(receiver, *method, method->isInstanceMethod());
}

auto objc_unwritten_property(const clang::ObjCIvarDecl& ivar) -> const clang::ObjCPropertyDecl* {
    const auto* implementation =
        llvm::dyn_cast<clang::ObjCImplementationDecl>(ivar.getDeclContext());
    if(!ivar.getSynthesize() || !implementation) {
        return nullptr;
    }
    const auto* synthesis = implementation->FindPropertyImplIvarDecl(ivar.getIdentifier());
    return synthesis && synthesis->getBeginLoc().isInvalid() ? synthesis->getPropertyDecl()
                                                             : nullptr;
}

auto objc_alias_names(const clang::ObjCCompatibleAliasDecl& alias)
    -> std::pair<clang::SourceLocation, clang::SourceLocation> {
    auto& context = alias.getASTContext();
    auto after = [&](clang::SourceLocation location) -> clang::SourceLocation {
        if(location.isInvalid()) {
            return {};
        }
        auto token = clang::Lexer::findNextToken(location,
                                                 context.getSourceManager(),
                                                 context.getLangOpts());
        return token ? token->getLocation() : clang::SourceLocation();
    };
    auto name = after(after(alias.getLocation()));
    return {name, after(name)};
}

auto objc_extension_paren(const clang::ObjCCategoryDecl& extension) -> clang::SourceLocation {
    auto& context = extension.getASTContext();
    auto paren = clang::Lexer::findNextToken(extension.getLocation(),
                                             context.getSourceManager(),
                                             context.getLangOpts());
    return paren && paren->is(clang::tok::l_paren) ? paren->getLocation() : clang::SourceLocation();
}

auto objc_selector_method(const clang::ObjCInterfaceDecl& objc_class, clang::Selector selector)
    -> const clang::ObjCMethodDecl* {
    if(!objc_class.hasDefinition()) {
        return nullptr;
    }
    for(bool instance: {true, false}) {
        if(const auto* method = objc_class.lookupPrivateMethod(selector, instance)) {
            return method;
        }
        if(const auto* method = objc_class.lookupMethod(selector, instance)) {
            return method;
        }
    }
    return nullptr;
}

auto objc_selector_pieces(const clang::ObjCSelectorExpr& expr, const clang::ASTContext& context)
    -> llvm::SmallVector<clang::SourceLocation, 4> {
    const auto& sources = context.getSourceManager();
    auto token_after = [&](clang::SourceLocation location) -> std::optional<clang::Token> {
        return location.isValid()
                   ? clang::Lexer::findNextToken(location, sources, context.getLangOpts())
                   : std::nullopt;
    };
    // `@`, `selector`, `(`, then a name and a `:` per piece, a name alone
    // for a selector with no argument.
    auto selector = expr.getSelector();
    auto token = token_after(expr.getAtLoc());
    for(int skipped = 0; skipped < 2 && token; skipped += 1) {
        token = token_after(token->getLocation());
    }
    llvm::SmallVector<clang::SourceLocation, 4> pieces;
    for(unsigned slot = 0; slot < std::max(1U, selector.getNumArgs()) && token; slot += 1) {
        if(token->is(clang::tok::raw_identifier)) {
            pieces.push_back(token->getLocation());
            token = token_after(token->getLocation());
        } else {
            pieces.emplace_back();
        }
        if(selector.getNumArgs() != 0 && token && token->is(clang::tok::colon)) {
            token = token_after(token->getLocation());
        }
    }
    return pieces;
}

auto objc_class_name(const clang::ObjCInterfaceDecl* objc_class, clang::SourceLocation location)
    -> const clang::NamedDecl* {
    if(!objc_class) {
        return nullptr;
    }
    auto& context = objc_class->getASTContext();
    const auto& sources = context.getSourceManager();
    auto spelling = sources.getSpellingLoc(location);
    auto [file, offset] = sources.getDecomposedLoc(spelling);
    auto text = sources.getBufferData(file).drop_front(offset);
    auto name = objc_class->getName();
    if(text.starts_with(name) &&
       (text.size() == name.size() || !clang::isAsciiIdentifierContinue(text[name.size()]))) {
        return objc_class;
    }
    auto written =
        text.take_front(clang::Lexer::MeasureTokenLength(spelling, sources, context.getLangOpts()));
    for(const auto* found: context.getTranslationUnitDecl()->lookup(&context.Idents.get(written))) {
        if(const auto* alias = llvm::dyn_cast<clang::ObjCCompatibleAliasDecl>(found)) {
            return alias;
        }
    }
    return objc_class;
}

auto objc_overridden(const clang::ObjCMethodDecl& method) -> const clang::ObjCMethodDecl* {
    const auto* objc_class = decls::objc_class(method.getDeclContext());
    for(const auto* base = objc_class ? objc_class->getSuperClass() : nullptr;
        base && base->hasDefinition();
        base = base->getSuperClass()) {
        if(const auto* own = own_method(*base, method.getSelector(), method.isInstanceMethod())) {
            return own;
        }
    }
    return nullptr;
}

auto constructor_of(const clang::CXXConstructExpr* construct) -> const clang::CXXConstructorDecl* {
    auto* constructor = construct->getConstructor();
    if(auto inherited = constructor->getInheritedConstructor()) {
        return inherited.getConstructor();
    }
    return constructor;
}

namespace {

template <typename TemplateDeclTy>
clang::NamedDecl* only_instantiation_impl(TemplateDeclTy* TD) {
    clang::NamedDecl* Only = nullptr;
    for(auto* Spec: TD->specializations()) {
        if(Spec->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization)
            continue;
        // A class template specialization the code only names, through a
        // pointer to it, is not instantiated; one a partial specialization
        // matches instantiates the partial specialization instead.
        if constexpr(std::is_same_v<TemplateDeclTy, clang::ClassTemplateDecl>) {
            if(Spec->getTemplateSpecializationKind() == clang::TSK_Undeclared ||
               llvm::isa<clang::ClassTemplatePartialSpecializationDecl*>(
                   Spec->getSpecializedTemplateOrPartial()))
                continue;
        }
        if(Only != nullptr)
            return nullptr;
        Only = Spec;
    }
    return Only;
}

}  // namespace

auto instantiations(const clang::FunctionTemplateDecl& templated)
    -> llvm::SmallVector<clang::FunctionDecl*, 4> {
    llvm::SmallVector<clang::FunctionDecl*, 4> calls;
    for(auto* specialization: templated.specializations()) {
        auto kind = specialization->getTemplateSpecializationKind();
        // Overload resolution specializes every function template candidate
        // it deduces; only the one a call picks is referenced, or left
        // invalid and unreferenced by a body that fails to instantiate for
        // it, as a generic lambda's deduced return type makes it.
        if(kind == clang::TSK_ExplicitSpecialization ||
           (kind == clang::TSK_ImplicitInstantiation && !specialization->isReferenced() &&
            !specialization->isInvalidDecl())) {
            continue;
        }
        calls.push_back(specialization);
    }
    return calls;
}

auto only_instantiation(clang::NamedDecl* TemplatedDecl) -> clang::NamedDecl* {
    assert(TemplatedDecl);
    if(auto* TD = TemplatedDecl->getDescribedTemplate()) {
        if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(TD))
            return only_instantiation_impl(CTD);
        if(auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(TD)) {
            auto calls = instantiations(*FTD);
            // A call whose body failed counts only when no call succeeds.
            auto failed = [](const clang::FunctionDecl* call) {
                return call->getTemplateSpecializationKind() == clang::TSK_ImplicitInstantiation &&
                       !call->isReferenced();
            };
            if(!ranges::all_of(calls, failed))
                llvm::erase_if(calls, failed);
            return calls.size() == 1 ? calls.front() : nullptr;
        }
        if(auto* VTD = llvm::dyn_cast<clang::VarTemplateDecl>(TD))
            return only_instantiation_impl(VTD);
    }
    return nullptr;
}

auto only_instantiation(clang::ParmVarDecl* decl) -> clang::ParmVarDecl* {
    assert(decl);
    auto* TemplateFunction = llvm::dyn_cast<clang::FunctionDecl>(decl->getDeclContext());
    if(!TemplateFunction)
        return nullptr;
    auto* InstantiatedFunction =
        llvm::dyn_cast_or_null<clang::FunctionDecl>(only_instantiation(TemplateFunction));
    if(!InstantiatedFunction)
        return nullptr;

    unsigned ParamIdx = 0;
    for(auto* Param: TemplateFunction->parameters()) {
        // Can't reason about param indexes in the presence of preceding packs.
        // And if this param is a pack, it may expand to multiple params.
        if(Param->isParameterPack())
            return nullptr;
        if(Param == decl)
            break;
        ++ParamIdx;
    }
    if(ParamIdx >= InstantiatedFunction->getNumParams()) {
        return nullptr;
    }
    return InstantiatedFunction->getParamDecl(ParamIdx);
}

namespace {

// Returns the template parameter pack type from an instantiated function
// template, if it exists, nullptr otherwise.
auto function_pack_type(const clang::FunctionDecl* callee) -> const clang::TemplateTypeParmType* {
    // returns true for `X` in `template <typename... X> void foo()`
    auto is_type_pack = [](clang::NamedDecl* decl) {
        if(const auto* TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            return TTPD->isParameterPack();
        }
        return false;
    };

    if(const auto* decl = callee->getPrimaryTemplate()) {
        auto template_params = decl->getTemplateParameters()->asArray();
        // find the template parameter pack from the back
        const auto it =
            ranges::find_if(template_params.rbegin(), template_params.rend(), is_type_pack);
        if(it != template_params.rend()) {
            const auto* TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(*it);
            return TTPD->getTypeForDecl()->castAs<clang::TemplateTypeParmType>();
        }
    }

    return nullptr;
}

}  // namespace

// Returns the template parameter pack type that this parameter was expanded
// from (if in the Args... or Args&... or Args&&... form), if this is the case,
// nullptr otherwise.
auto underlying_pack_type(const clang::ParmVarDecl* param) -> const clang::TemplateTypeParmType* {
    assert(param);
    const auto* type = param->getType().getTypePtr();
    if(auto* ref_type = llvm::dyn_cast<clang::ReferenceType>(type)) {
        type = ref_type->getPointeeTypeAsWritten().getTypePtr();
    }

    if(const auto* subst_type = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(type)) {
        const auto* decl = replaced_parameter(*subst_type);
        if(decl && decl->isParameterPack()) {
            return decl->getTypeForDecl()->castAs<clang::TemplateTypeParmType>();
        }
    }

    return nullptr;
}

namespace {

bool is_std_forward(const clang::FunctionDecl* callee) {
    auto* name = callee->getIdentifier();
    return name && name->getName() == "forward" && callee->isInStdNamespace();
}

/// The parameter `arg` passes on unchanged: named directly, through
/// `std::forward`, or through the copy or move building a by-value
/// parameter, implicit nodes aside.
const clang::ParmVarDecl* passed_param(const clang::Expr* arg) {
    arg = arg->IgnoreImplicitAsWritten();
    if(auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(arg);
       construct && construct->getConstructor()->isCopyOrMoveConstructor()) {
        arg = construct->getArg(0)->IgnoreImplicitAsWritten();
    }
    if(auto* call = llvm::dyn_cast<clang::CallExpr>(arg)) {
        if(auto* callee = call->getDirectCallee();
           callee && is_std_forward(callee) && call->getNumArgs() == 1) {
            arg = call->getArg(0)->IgnoreImplicitAsWritten();
        }
    }
    auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(arg);
    return ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
}

/// The run of `params`, parameters of `fn`, that `fn`'s trailing type
/// parameter pack expanded to; empty, at the end of `params`, when none.
auto expanded_pack(const clang::FunctionDecl* fn, llvm::ArrayRef<const clang::ParmVarDecl*> params)
    -> llvm::ArrayRef<const clang::ParmVarDecl*> {
    auto* pack = function_pack_type(fn);
    auto expanded = [pack](const clang::ParmVarDecl* param) {
        return pack && underlying_pack_type(param) == pack;
    };
    return params.drop_until(expanded).take_while(expanded);
}

/// The function `call` calls; for a call left dependent on an overloaded
/// name, the only candidate taking as many arguments.
const clang::FunctionDecl* callee_of(const clang::CallExpr* call) {
    if(auto* fn = call->getDirectCallee()) {
        return fn;
    }
    auto* lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(call->getCallee());
    if(!lookup || lookup->requiresADL()) {
        return nullptr;
    }
    const clang::FunctionDecl* unique = nullptr;
    for(auto* candidate: lookup->decls()) {
        auto* fn = llvm::dyn_cast<clang::FunctionDecl>(candidate);
        if(!fn || fn->getNumParams() != call->getNumArgs()) {
            continue;
        }
        if(unique) {
            return nullptr;
        }
        unique = fn;
    }
    return unique;
}

/// Finds the first call in a function body that passes `pack`, parameters
/// of that function, on as consecutive arguments, and the callee
/// parameters receiving them. For example, in
///
///   template <typename T, typename... Args>
///   auto make_unique(Args... args) {
///     return unique_ptr<T>(new T(args...));
///   }
///
/// `make_unique<std::string>(2, 'x')` passes its two parameters on to the
/// constructor `std::string(int, char)`.
struct PackForwardFinder : clang::RecursiveASTVisitor<PackForwardFinder> {
    llvm::ArrayRef<const clang::ParmVarDecl*> pack;

    const clang::FunctionDecl* callee = nullptr;
    llvm::ArrayRef<const clang::ParmVarDecl*> params;

    bool VisitCallExpr(clang::CallExpr* call) {
        // `std::forward` hands its argument on to the call around it.
        auto* fn = callee_of(call);
        if(!fn || is_std_forward(fn)) {
            return true;
        }
        llvm::ArrayRef<const clang::Expr*> args(call->getArgs(), call->getNumArgs());
        // The object a member operator is called on binds a parameter only
        // when it is the explicit object.
        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(fn);
           method && !method->isExplicitObjectMemberFunction() &&
           llvm::isa<clang::CXXOperatorCallExpr>(call)) {
            args = args.drop_front();
        }
        match(fn, args);
        return !callee;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* construct) {
        match(construct->getConstructor(), {construct->getArgs(), construct->getNumArgs()});
        return !callee;
    }

    /// `args` bind `fn`'s parameters in order.
    void match(const clang::FunctionDecl* fn, llvm::ArrayRef<const clang::Expr*> args) {
        // Positions line up only when every parameter has its argument,
        // defaults included; the arguments past them go to a C variadic
        // `...`. A dependent call may bind otherwise (an explicit object
        // left out of its arguments).
        auto named = fn->getNumParams();
        if(args.size() < named || (args.size() > named && !fn->isVariadic())) {
            return;
        }
        args = args.take_front(named);
        // A written expansion, left in a dependent call, stands for any
        // number of arguments: the ones after it bind unknown parameters.
        if(ranges::any_of(args, llvm::IsaPred<clang::PackExpansionExpr>)) {
            return;
        }
        auto passes = [](const clang::Expr* arg, const clang::ParmVarDecl* param) {
            return passed_param(arg) == param;
        };
        for(std::size_t start = 0; start + pack.size() <= args.size(); start += 1) {
            if(ranges::equal(args.slice(start, pack.size()), pack, passes)) {
                callee = fn;
                params = fn->parameters().slice(start, pack.size());
                return;
            }
        }
    }
};

}  // namespace

auto resolve_forwarding_params(const clang::FunctionDecl* decl, unsigned max_depth)
    -> llvm::SmallVector<const clang::ParmVarDecl*> {
    assert(decl);
    llvm::ArrayRef<const clang::ParmVarDecl*> own = decl->parameters();
    llvm::SmallVector<const clang::ParmVarDecl*> resolved(own.begin(), own.end());
    // The pack still being followed, as parameters of `function`, and where
    // its first element stands in `resolved`.
    const clang::FunctionDecl* function = decl;
    auto pack = expanded_pack(decl, own);
    std::size_t offset = pack.data() - own.data();
    llvm::SmallPtrSet<const clang::FunctionTemplateDecl*, 4> seen;
    if(auto* primary = decl->getPrimaryTemplate()) {
        seen.insert(primary);
    }
    for(unsigned depth = 0; !pack.empty() && depth < max_depth; depth += 1) {
        PackForwardFinder finder{.pack = pack};
        finder.TraverseStmt(function->getBody());
        if(!finder.callee) {
            break;
        }
        // A template reached again recurses over its own pack (`f(rest...)`
        // peeling one argument per step): no parameter is its target.
        if(auto* primary = finder.callee->getPrimaryTemplate();
           primary && !seen.insert(primary).second) {
            return {own.begin(), own.end()};
        }
        ranges::copy(finder.params, resolved.begin() + offset);
        auto next = expanded_pack(finder.callee, finder.params);
        offset += next.data() - finder.params.data();
        pack = next;
        function = finder.callee;
    }
    return resolved;
}

bool binds_mutable_reference(const clang::ParmVarDecl* param, const clang::ParmVarDecl* forwarded) {
    auto forwarded_type = forwarded->getType();
    return param->getType()->isLValueReferenceType() && forwarded_type->isLValueReferenceType() &&
           !forwarded_type.getNonReferenceType().isConstQualified() &&
           !underlying_pack_type(forwarded);
}

bool ParameterBinding::mutable_reference() const {
    // A reference collapsed from `T&&` or `auto&&` is not spelled as one.
    return binds_mutable_reference(param, forwarded) &&
           forwarded->getType()->castAs<clang::LValueReferenceType>()->isSpelledAsLValue();
}

auto parameter_bindings(const clang::Stmt* call) -> llvm::SmallVector<ParameterBinding> {
    const clang::FunctionDecl* callee = nullptr;
    llvm::ArrayRef<const clang::Expr*> args;
    if(const auto* expr = llvm::dyn_cast<clang::CallExpr>(call)) {
        callee = expr->getDirectCallee();
        args = {expr->getArgs(), expr->getNumArgs()};
    } else if(const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(call)) {
        callee = construct->getConstructor();
        args = {construct->getArgs(), construct->getNumArgs()};
    }
    if(!callee) {
        return {};
    }

    // The object a call is made on leads the arguments of an operator call
    // and of a call to an explicit object member function; the parameters
    // list it unless the callee is an implicit object member.
    const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
    bool explicit_object = method && method->isExplicitObjectMemberFunction();
    std::size_t skipped_params = 0;
    if(llvm::isa<clang::CXXOperatorCallExpr>(call)) {
        args = args.drop_front();
        if(!method || explicit_object) {
            skipped_params = 1;
        }
    } else if(explicit_object) {
        args = args.drop_front();
        skipped_params = 1;
    }

    llvm::SmallVector<ParameterBinding> bindings;
    llvm::SmallVector<const clang::ParmVarDecl*> forwarded_params;
    for(auto [offset, arg]: views::enumerate(args)) {
        auto position = skipped_params + static_cast<std::size_t>(offset);
        if(position >= callee->getNumParams()) {
            break;
        }
        const auto* param = callee->getParamDecl(position);
        const auto* forwarded = param;
        if(underlying_pack_type(param)) {
            if(forwarded_params.empty()) {
                forwarded_params = resolve_forwarding_params(callee);
            }
            forwarded = forwarded_params[position];
        }
        bindings.push_back({.argument = arg, .param = param, .forwarded = forwarded});
    }
    return bindings;
}

auto explicit_template_arguments(const clang::Expr* callee)
    -> llvm::ArrayRef<clang::TemplateArgumentLoc> {
    if(const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(callee)) {
        return ref->template_arguments();
    }
    if(const auto* member = llvm::dyn_cast<clang::MemberExpr>(callee)) {
        return member->template_arguments();
    }
    if(const auto* overload = llvm::dyn_cast<clang::OverloadExpr>(callee)) {
        return overload->template_arguments();
    }
    if(const auto* ref = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(callee)) {
        return ref->template_arguments();
    }
    if(const auto* member = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(callee)) {
        return member->template_arguments();
    }
    return {};
}

}  // namespace clice::decls
