#include "AST/Resolver.h"

#include "Support/Format.h"

#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Sema/TreeTransform.h"

namespace clice {

namespace {

template <typename Callback>
void visitTemplateDeclContexts(clang::Decl* decl, const Callback& callback) {
    while(true) {
        if(llvm::isa<clang::TranslationUnitDecl>(decl)) {
            break;
        }

        clang::TemplateParameterList* params = nullptr;

        if(auto TD = decl->getDescribedTemplate()) {
            params = TD->getTemplateParameters();
        }

        if(auto CTPSD = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            params = CTPSD->getTemplateParameters();
        }

        if(auto VTPSD = llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            params = VTPSD->getTemplateParameters();
        }

        if(params) {
            callback(decl, params);
        }

        decl = llvm::dyn_cast<clang::Decl>(decl->getDeclContext());
    }
}

/// `Sema::SubstType` will not substitute template arguments in aliased types.
/// For example:
///
/// ```cpp
/// template <typename T>
/// struct A {
///     using base = std::vector<T>;
///     using type = typename base::reference;
/// };
/// ```
///
/// In this case, if you call `SubstType` on `type`, the alias `base` will remain with
/// the original type parameter `T`, without substituting it. Therefore, we need to
/// manually resolve the alias before calling `SubstType`, which is what `DesugarOnly`
/// aims to achieve.
class DesugarOnly : public clang::TreeTransform<DesugarOnly> {
    using Base = clang::TreeTransform<DesugarOnly>;

public:
    DesugarOnly(clang::Sema& sema) : Base(sema), context(sema.getASTContext()) {}

    // FIXME: desugar more types, e.g `UsingType`.

    clang::QualType TransformTypedefType(clang::TypeLocBuilder& TLB, clang::TypedefTypeLoc TL) {
        if(clang::TypedefNameDecl* TND = TL.getTypedefNameDecl()) {
            auto type = TransformType(TND->getUnderlyingType());
            if(auto ET = llvm::dyn_cast<clang::ElaboratedType>(type)) {
                type = ET->getNamedType();
            }
            TLB.pushTrivial(context, type, {});
            return type;
        }

        return clang::QualType();
    }

    clang::QualType TransformElaboratedType(clang::TypeLocBuilder& TLB,
                                            clang::ElaboratedTypeLoc TL) {
        clang::QualType type = TransformType(TL.getNamedTypeLoc().getType());
        TLB.pushTrivial(context, type, {});
        return type;
    }

    clang::QualType TransformInjectedClassNameType(clang::TypeLocBuilder& TLB,
                                                   clang::InjectedClassNameTypeLoc TL) {
        auto ICT = TL.getTypePtr();
        clang::QualType type = TransformType(ICT->getInjectedSpecializationType());
        TLB.pushTrivial(context, type, {});
        return type;
    }

    using Base::TransformTemplateSpecializationType;

    clang::QualType TransformTemplateSpecializationType(clang::TypeLocBuilder& TLB,
                                                        clang::TemplateSpecializationTypeLoc TL) {
        if(TL.getTypePtr()->isTypeAlias()) {
            clang::QualType type = TransformType(TL.getTypePtr()->desugar());
            TLB.pushTrivial(context, type, {});
            return type;
        }
        return Base::TransformTemplateSpecializationType(TLB, TL);
    }

private:
    clang::ASTContext& context;
};

/// When deduce `TemplateSpecializationType` for partial specialization, `TemplateTypeParmType`
/// will be deduced as canonical type.
///
/// For example:
/// ```cpp
/// template <typename T>
/// struct A {};
///
/// template <typename T>
/// struct A<T*> {};
/// ```
/// If you use deduce `A` with `A<U*>`, you will get `T = type-parameter-0-0` instead of `U`.
///
/// For code completion, we don't care about the whether it's a canonical type or not. But
/// sometimes, the type may be needed to display to the user, e.g. inlay hints. In this case,
/// we need to resugar the type to make it more readable.
class ResugarOnly : public clang::TreeTransform<ResugarOnly> {
public:
    ResugarOnly(clang::Sema& sema, clang::Decl* decl) :
        TreeTransform(sema), context(sema.getASTContext()) {
        visitTemplateDeclContexts(decl,
                                  [&](clang::Decl* decl, clang::TemplateParameterList* params) {
                                      lists.push_back(params);
                                  });
        std::ranges::reverse(lists);
    }

    clang::QualType TransformTemplateTypeParmType(clang::TypeLocBuilder& TLB,
                                                  clang::TemplateTypeParmTypeLoc TL,
                                                  bool = false) {
        clang::QualType type = TL.getType();
        auto TTPT = TL.getTypePtr();
        if(!TTPT->getDecl()) {
            auto depth = TTPT->getDepth();
            auto index = TTPT->getIndex();
            auto isPack = TTPT->isParameterPack();
            auto param = llvm::cast<clang::TemplateTypeParmDecl>(lists[depth]->getParam(index));
            type = context.getTemplateTypeParmType(depth, index, isPack, param);
        }
        return TLB.push<clang::TemplateTypeParmTypeLoc>(type).getType();
    }

private:
    clang::ASTContext& context;
    llvm::SmallVector<clang::TemplateParameterList*> lists;
};

/// A helper class to record the instantiation stack.
struct InstantiationStack {
    using Arguments = llvm::SmallVector<clang::TemplateArgument, 4>;
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    llvm::SmallVector<std::pair<clang::Decl*, Arguments>> data;

    void clear() {
        data.clear();
    }

    bool empty() const {
        return data.empty();
    }

    auto state() const {
        return data;
    }

    void rewind(auto& point) {
        data = std::move(point);
    }

    void push(clang::Decl* decl, TemplateArguments arguments) {
        data.emplace_back(decl, arguments);
    }

    void pop() {
        data.pop_back();
    }

    auto& frames() {
        return data;
    }
};

/// The core class that performs pseudo template instantiation.
class PseudoInstantiator : public clang::TreeTransform<PseudoInstantiator> {
public:
    using Base = clang::TreeTransform<PseudoInstantiator>;

    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    using TemplateDeductionInfo = clang::sema::TemplateDeductionInfo;

    PseudoInstantiator(clang::Sema& sema, llvm::DenseMap<const void*, clang::QualType>& resolved) :
        Base(sema), sema(sema), context(sema.getASTContext()), resolved(resolved) {}

public:
    /// Check whether the given template arguments match the template parameters and
    /// complete the default template arguments if necessary.
    bool checkTemplateArguments(clang::TemplateDecl* TD,
                                TemplateArguments& arguments,
                                llvm::SmallVectorImpl<clang::TemplateArgument>& out) {
        auto list = TD->getTemplateParameters();
        out.reserve(list->size());
        for(auto arg: arguments) {
            out.emplace_back(arg);
        }

        if(out.size() != list->size()) {
            for(auto i = out.size(); i < list->size(); ++i) {
                auto param = list->getParam(i);
                auto TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param);
                if(TTPD && TTPD->hasDefaultArgument()) {
                    auto type = TTPD->getDefaultArgument().getArgument().getAsType();

                    auto state = stack.state();

                    stack.push(TD, out);

                    auto result = TransformType(instantiate(type));

                    if(result.isNull()) {
                        stack.rewind(state);
                        return false;
                    }

                    out.emplace_back(result);
                    stack.rewind(state);
                }
            }
        }

        if(out.size() != list->size()) {
            return false;
        }

        return true;
    }

    /// Deduce the template arguments for the given declaration. If deduction succeeds, push the
    /// declaration and its deduced template arguments to the instantiation stack.
    template <typename Decl>
    bool deduceTemplateArguments(Decl* decl, TemplateArguments arguments) {
        clang::TemplateParameterList* list = nullptr;
        TemplateArguments params = {};

        if constexpr(std::is_same_v<Decl, clang::ClassTemplateDecl>) {
            const clang::ClassTemplateDecl* CTD = decl;
            list = CTD->getTemplateParameters();
            params = list->getInjectedTemplateArgs(context);
        } else if constexpr(std::is_same_v<Decl, clang::ClassTemplatePartialSpecializationDecl>) {
            const clang::ClassTemplatePartialSpecializationDecl* CTPSD = decl;
            list = CTPSD->getTemplateParameters();
            params = CTPSD->getTemplateArgs().asArray();
        } else if constexpr(std::is_same_v<Decl, clang::TypeAliasTemplateDecl>) {
            const clang::TypeAliasTemplateDecl* TATD = decl;
            list = TATD->getTemplateParameters();
            params = list->getInjectedTemplateArgs(context);
        } else {
            static_assert(dependent_false<Decl>, "Unknown declaration type");
        }

        assert(list && "No template parameters found");

        TemplateDeductionInfo info = {clang::SourceLocation(), list->getDepth()};
        llvm::SmallVector<clang::DeducedTemplateArgument, 4> deduced(list->size());

        auto result = sema.DeduceTemplateArguments(list, params, arguments, info, deduced, true);
        bool success =
            result == clang::TemplateDeductionResult::Success && !info.hasSFINAEDiagnostic();

        if(!success) {
            return false;
        }

        /// made up class template context.
        if(stack.empty()) {
            visitTemplateDeclContexts(llvm::dyn_cast<clang::Decl>(decl->getDeclContext()),
                                      [&](clang::Decl* decl, clang::TemplateParameterList* params) {
                                          stack.push(decl,
                                                     params->getInjectedTemplateArgs(context));
                                      });
            std::ranges::reverse(stack.frames());
        }

        llvm::SmallVector<clang::TemplateArgument, 4> output(deduced.begin(), deduced.end());
        stack.push(decl, output);

        return true;
    }

    using lookup_result = clang::DeclContext::lookup_result;

    /// If this class and its base class have members with the same name, `DeclContext::lookup`
    /// will return multiple declarations in order from the base class to the derived class, so we
    /// use the last declaration.
    clang::Decl* preferred(lookup_result members) {
        clang::Decl* decl = nullptr;
        std::ranges::for_each(members, [&](auto member) { decl = member; });
        return decl;
    }

    lookup_result lookup(clang::QualType type, clang::DeclarationName name) {
        clang::Decl* TD = nullptr;
        llvm::ArrayRef<clang::TemplateArgument> args;
        type = TransformType(type);

        if(type.isNull()) {
            return lookup_result();
        }

        if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
            TD = TST->getTemplateName().getAsTemplateDecl();
            args = TST->template_arguments();
        } else if(auto DTST = type->getAs<clang::DependentTemplateSpecializationType>()) {
            auto& template_name = DTST->getDependentTemplateName();
            /// FIXME: operators does't have the name.
            auto name = template_name.getName().getIdentifier();
            if(!name) {
                return {};
            }

            if(auto decl = preferred(lookup(template_name.getQualifier(), name))) {
                TD = decl;
                args = DTST->template_arguments();
            }
        }

        if(!TD) {
            return lookup_result();
        }

#ifndef NDEBUG
        if(TemplateResolver::debug) {
            llvm::outs() << "--------------------------------------------------------------\n";
            llvm::outs() << "lookup: { " << name << " } in { " << type.getAsString() << " }\n";
        }
#endif

        if(auto CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(TD)) {
            return lookup(CTD, name, args);
        } else if(auto TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(TD)) {
            if(deduceTemplateArguments(TATD, args)) {
                return lookup(instantiate(TATD->getTemplatedDecl()->getUnderlyingType()), name);
            }
        }

        return lookup_result();
    }

    /// Look up the name in the given nested name specifier.
    lookup_result lookup(const clang::NestedNameSpecifier* NNS, clang::DeclarationName name) {
        if(!NNS) {
            return lookup_result();
        }

        /// Search the resolved entities first.
        if(auto iter = resolved.find(NNS); iter != resolved.end()) {
            return lookup(iter->second, name);
        }

        switch(NNS->getKind()) {
            case clang::NestedNameSpecifier::Identifier: {
                /// If the prefix is `Identifier`, it must be a dependent name.
                /// For example: `std::vector<T>::value_type::type`
                ///               ^~~~~~~~~~~~~~~~~~~~~~~~~^
                ///                        identifier
                ///
                /// So resolve it recursively.
                auto type =
                    instantiate(preferred(lookup(NNS->getPrefix(), NNS->getAsIdentifier())));
                resolved.try_emplace(NNS, type);
                return lookup(type, name);
            }

            case clang::NestedNameSpecifier::TypeSpec: {
                /// If the prefix is `TypeSpec`, it must be a type.
                return lookup(clang::QualType(NNS->getAsType(), 0), name);
            }

            case clang::NestedNameSpecifier::Global:
            case clang::NestedNameSpecifier::Namespace:
            case clang::NestedNameSpecifier::NamespaceAlias:
            case clang::NestedNameSpecifier::Super: {
                llvm::errs() << "Unexpected name specifier\n";
                std::abort();
            }
        }
    }

    /// Look up the name in the bases of the given class. Keep stack unchanged.
    lookup_result lookupInBases(clang::CXXRecordDecl* CRD, clang::DeclarationName name) {
        if(!CRD->hasDefinition()) {
            return lookup_result();
        }

        for(auto base: CRD->bases()) {
            if(auto type = base.getType(); type->isDependentType()) {
                auto state = stack.state();
                if(auto members = lookup(instantiate(type), name); !members.empty()) {
                    return members;
                }
                stack.rewind(state);
            }
        }

        return lookup_result();
    }

    /// Look up the name in the given class template. We first search the name in the
    /// primary template, if failed, try dependent base classes, if still failed, try
    /// partial specializations. **Note that this function will be responsible for pushing
    /// the class template and its template arguments to the instantiation stack**.
    lookup_result lookup(clang::ClassTemplateDecl* CTD,
                         clang::DeclarationName name,
                         TemplateArguments visibleArguments) {
        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        if(!checkTemplateArguments(CTD, visibleArguments, arguments)) {
            return lookup_result();
        }

        /// Try to find the name in the partial specializations.
        llvm::SmallVector<clang::ClassTemplatePartialSpecializationDecl*> partials;
        CTD->getPartialSpecializations(partials);

        for(auto partial: partials) {
            if(deduceTemplateArguments(partial, arguments)) {
                if(auto members = partial->lookup(name); !members.empty()) {
                    return members;
                }

                if(auto members = lookupInBases(partial, name); !members.empty()) {
                    return members;
                }

                stack.pop();
            }
        }

        if(deduceTemplateArguments(CTD, arguments)) {
            auto CRD = CTD->getTemplatedDecl();
            /// First, try to find the name in the primary template.
            if(auto members = CRD->lookup(name); !members.empty()) {
                return members;
            }

            /// If failed, try to find the name in the dependent base classes.
            if(auto members = lookupInBases(CRD, name); !members.empty()) {
                return members;
            }

            /// If failed, pop the decl and deduced template arguments.
            stack.pop();
        }

        /// FIXME: try full specializations?.

        return lookup_result();
    }

    /// Instantiate the given type and clear the instantiation stack.
    clang::QualType instantiate(clang::QualType type) {
        if(!type->isDependentType()) {
            return type;
        }

        auto& contexts = sema.CodeSynthesisContexts;
        assert(contexts.empty() && "CodeSynthesisContexts should be empty");
        assert(!stack.frames().empty() && "Instantiation stack should not be empty");

        std::ranges::for_each(stack.frames(), [&](auto& frame) {
            clang::Sema::CodeSynthesisContext context;
            context.Entity = frame.first;
            context.TemplateArgs = frame.second.data();
            context.Kind = clang::Sema::CodeSynthesisContext::TemplateInstantiation;
            contexts.push_back(context);
        });

        clang::MultiLevelTemplateArgumentList list;
        std::ranges::for_each(stack.frames() | std::ranges::views::reverse, [&](auto& frame) {
            list.addOuterTemplateArguments(frame.first, frame.second, true);
        });

        type = DesugarOnly(sema).TransformType(type);

#ifndef NDEBUG
        if(TemplateResolver::debug) {
            llvm::SmallString<128> args;
            for(auto& frame: stack.frames()) {
                args += "<";
                for(auto& arg: frame.second) {
                    switch(arg.getKind()) {
                        case clang::TemplateArgument::Null:
                        case clang::TemplateArgument::Type: {
                            args += arg.getAsType().getAsString();
                            break;
                        }
                        case clang::TemplateArgument::Declaration: {
                            args += arg.getAsDecl()->getNameAsString();
                            break;
                        }
                        case clang::TemplateArgument::NullPtr: {
                            args += "nullptr";
                            break;
                        }
                        case clang::TemplateArgument::Integral: {
                            arg.getAsIntegral().toString(args, 10);
                            break;
                        }
                        case clang::TemplateArgument::StructuralValue: {
                            args += arg.getAsStructuralValue().getAsString(
                                context,
                                arg.getStructuralValueType());
                            break;
                        }
                        case clang::TemplateArgument::Template: {
                            args += arg.getAsTemplate().getAsTemplateDecl()->getNameAsString();
                            break;
                        }
                        case clang::TemplateArgument::TemplateExpansion: {
                            args += arg.getAsTemplateOrTemplatePattern()
                                        .getAsTemplateDecl()
                                        ->getNameAsString();
                            break;
                        }
                        case clang::TemplateArgument::Expression: {
                            args += arg.getAsExpr()->getStmtClassName();
                            break;
                        }
                        case clang::TemplateArgument::Pack: {
                            args += "pack";
                            break;
                        }
                    }
                    args += ", ";
                }
                args += ">, ";
            }
            print("try instantiate [{}] with arguments: {}\n", type.getAsString(), args.str());
        }

#endif

        auto result = sema.SubstType(type, list, {}, {});

#ifndef NDEBUG
        if(TemplateResolver::debug) {
            print("instantiate success: {}\n", result.getAsString());
        }
#endif

        stack.clear();
        contexts.clear();

        return result;
    }

    clang::QualType instantiate(clang::Decl* decl) {
        if(!decl) {
            return clang::QualType();
        }

        if(auto TND = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
            return instantiate(TND->getUnderlyingType());
        } else if(auto RD = llvm::dyn_cast<clang::RecordDecl>(decl)) {
            return clang::QualType(RD->getTypeForDecl(), 0);
        }

        // FIXME: more possibilities?

        return clang::QualType();
    }

    /// FIXME: Use a general method to dig holes.
    clang::QualType hole(clang::NestedNameSpecifier* NNS,
                         const clang::IdentifierInfo* member,
                         TemplateArguments arguments) {
        if(NNS->getKind() != clang::NestedNameSpecifier::TypeSpec) {
            return clang::QualType();
        }

        auto TST = NNS->getAsType()->getAs<clang::TemplateSpecializationType>();
        if(!TST) {
            return clang::QualType();
        }

        auto TD = TST->getTemplateName().getAsTemplateDecl();
        if(!TD->getDeclContext()->isStdNamespace()) {
            return clang::QualType();
        }

        if(TD->getName() == "allocator_traits") {
            assert(TST->template_arguments().size() == 1 && "Invalid template arguments");
            auto Alloc = TST->template_arguments()[0].getAsType();

            if(member->getName() == "rebind_alloc") {
                auto T = arguments[0].getAsType();

                /// Alloc::rebind<T>::other
                auto prefix =
                    clang::NestedNameSpecifier::Create(context, nullptr, Alloc.getTypePtr());

                auto rebind = sema.getPreprocessor().getIdentifierInfo("rebind");

                auto DTST = context.getDependentTemplateSpecializationType(
                    clang::ElaboratedTypeKeyword::None,
                    clang::DependentTemplateStorage(prefix, rebind, false),
                    arguments);

                prefix = clang::NestedNameSpecifier::Create(context, prefix, DTST.getTypePtr());

                auto other = sema.getPreprocessor().getIdentifierInfo("other");
                auto DNT = context.getDependentNameType(clang::ElaboratedTypeKeyword::Typename,
                                                        prefix,
                                                        other);

                auto result = PseudoInstantiator(sema, resolved).TransformType(DNT);
                if(!result.isNull()) {
                    return result;
                }

                /// SomeAllocator<U, Args> -> SomeAllocator<T, Args>
                if(auto TST = Alloc->getAs<clang::TemplateSpecializationType>()) {
                    llvm::SmallVector<clang::TemplateArgument, 1> replaceArguments = {T};
                    llvm::SmallVector<clang::TemplateArgument, 1> canonicalArguments;
                    for(auto& arg: replaceArguments) {
                        canonicalArguments.emplace_back(context.getCanonicalTemplateArgument(arg));
                    }
                    return context.getTemplateSpecializationType(TST->getTemplateName(),
                                                                 replaceArguments,
                                                                 canonicalArguments);
                }
            }
        }

        return clang::QualType();
    }

public:
    using Base::TransformType;

    clang::QualType TransformType(clang::QualType type) {
        if(type.isNull()) {
            return clang::QualType();
        }

        if(!type->isDependentType()) {
            return type;
        }

        return Base::TransformType(DesugarOnly(sema).TransformType(type));
    }

    /// Sometimes the outer argument is just a simple type `T` and actually cannot make
    /// instantiation continue. In this case, we try to use its default argument to replace it,
    /// which may make the instantiation continue.
    /// For example: `template <typename T = std::vector<T>> using type = T::value_type`.
    clang::QualType TransformTemplateTypeParmType(clang::TypeLocBuilder& TLB,
                                                  clang::TemplateTypeParmTypeLoc TL,
                                                  bool = false) {
        if(clang::TemplateTypeParmDecl* TTPD = TL.getDecl()) {
            if(TTPD->hasDefaultArgument()) {
                const clang::TemplateArgument& argument = TTPD->getDefaultArgument().getArgument();
                clang::QualType type = TransformType(argument.getAsType());
                TLB.pushTrivial(context, type, clang::SourceLocation());
                return type;
            }
        }

        TLB.push<clang::TemplateTypeParmTypeLoc>(TL.getType());
        return TL.getType();
    }

    clang::QualType TransformDependentNameType(clang::TypeLocBuilder& TLB,
                                               clang::DependentNameTypeLoc TL,
                                               bool DeducedTSTContext = false) {
        auto DNT = TL.getTypePtr();

        /// Search the resolved entities first.
        if(auto iter = resolved.find(DNT); iter != resolved.end()) {
            TLB.pushTrivial(context, iter->second, {});
            return iter->second;
        }

        auto NNS = TransformNestedNameSpecifierLoc(TL.getQualifierLoc()).getNestedNameSpecifier();
        auto type = TransformType(instantiate(preferred(lookup(NNS, DNT->getIdentifier()))));
        resolved.try_emplace(DNT, type);
        TLB.pushTrivial(context, type, {});
        return type;
    }

    using Base::TransformDependentTemplateSpecializationType;

    /// For a `DependentTemplateSpecializationType`, the template name can be either an alias
    /// template or a class template. If it is an alias template, we can simplify it directly
    /// by transforming the alias template's underlying type. However, if it is a class
    /// template, we need additional context (e.g., suffix name) to simplify it correctly. In
    /// this case, we defer further transformation to `TransformDependentNameType`, which can
    /// handle this scenario. Thus, if the template is not an alias template, we keep it
    /// unchanged here.
    clang::QualType TransformDependentTemplateSpecializationType(
        clang::TypeLocBuilder& TLB,
        clang::DependentTemplateSpecializationTypeLoc TL) {
        auto DTST = TL.getTypePtr();

        /// Search the resolved entities first.
        if(auto iter = resolved.find(DTST); iter != resolved.end()) {
            TLB.pushTrivial(context, iter->second, {});
            return iter->second;
        }

        auto NNS = TransformNestedNameSpecifierLoc(TL.getQualifierLoc()).getNestedNameSpecifier();
        if(!NNS) {
            return clang::QualType();
        }

        /// FIXME: figure out here.
        clang::TemplateArgumentListInfo info;
        using iterator = clang::TemplateArgumentLocContainerIterator<
            clang::DependentTemplateSpecializationTypeLoc>;
        TransformTemplateArguments(iterator(TL, 0), iterator(TL, TL.getNumArgs()), info);

        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        for(auto& arg: info.arguments()) {
            arguments.push_back(arg.getArgument());
        }

        /// FIXME: operator does't have a name.
        auto name = DTST->getDependentTemplateName().getName().getIdentifier();
        if(!name) {
            return clang::QualType();
        }

        /// Try resolve the hole.
        if(auto result = hole(NNS, name, arguments); !result.isNull()) {
            resolved.try_emplace(DTST, result);
            TLB.pushTrivial(context, result, {});
            return result;
        }

        /// The `lookup` may change the instantiation stack, save the current state.
        auto state = stack.state();
        if(auto decl = preferred(lookup(NNS, name))) {
            /// FIXME: Current implementation results in duplicated lookup.
            /// Cache the result of `lookup` to avoid duplicated lookup.
            if(auto TATD = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
                if(deduceTemplateArguments(TATD, DTST->template_arguments())) {
                    clang::QualType type =
                        TransformType(instantiate(TATD->getTemplatedDecl()->getUnderlyingType()));
                    resolved.try_emplace(DTST, type);
                    TLB.pushTrivial(context, type, {});
                    return type;
                }
            }
            stack.rewind(state);
        }

        /// FIXME: figure out here.
        auto result = context.getDependentTemplateSpecializationType(
            DTST->getKeyword(),
            clang::DependentTemplateStorage(NNS, name, false),
            arguments);

        return TLB.push<clang::DependentTemplateSpecializationTypeLoc>(result).getType();
    }

    /// FIXME: handle more cases.
    clang::QualType TransformDecltypeType(clang::TypeLocBuilder& TLB, clang::DecltypeTypeLoc TL) {
        auto expr = TL.getTypePtr()->getUnderlyingExpr();
        if(auto DRE = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
            if(auto decl = DRE->getDecl(); llvm::isa<clang::VarDecl>(decl)) {
                auto type = TransformType(decl->getType());
                TLB.pushTrivial(context, type, {});
                return type;
            }
        }

        return Base::TransformDecltypeType(TLB, TL);
    }

private:
    clang::Sema& sema;
    clang::ASTContext& context;
    InstantiationStack stack;
    llvm::DenseMap<const void*, clang::QualType>& resolved;
};

}  // namespace

clang::QualType TemplateResolver::resolve(clang::QualType type) {
    PseudoInstantiator instantiator(sema, resolved);
    return instantiator.TransformType(type);
}

clang::QualType TemplateResolver::resugar(clang::QualType type, clang::Decl* decl) {
    ResugarOnly resugar(sema, decl);
    return resugar.TransformType(type);
}

TemplateResolver::lookup_result TemplateResolver::lookup(const clang::NestedNameSpecifier* NNS,
                                                         clang::DeclarationName name) {
    PseudoInstantiator instantiator(sema, resolved);
    return instantiator.lookup(NNS, name);
}

}  // namespace clice
