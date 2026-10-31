module clice;

import :feature.code_actions.action;
import :feature.code_actions;
import :feature.code_completion.item;
import :feature.code_completion;
import :feature.documentation;
import :index.search;
import :semantic.decls;
import :semantic.display;
import :semantic.resolver;
import :semantic.semantics;
import :support.fuzzy_matcher;
import :support.markup;
import :syntax.completion;
import :syntax.documentation;
import :syntax.lexer;

namespace clice::feature {

namespace {

auto completion_kind(const clang::NamedDecl* decl) -> protocol::CompletionItemKind {
    if(auto* function = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
        decl = function->getTemplatedDecl();
    }

    if(llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(decl)) {
        return protocol::CompletionItemKind::Module;
    }

    if(llvm::isa<clang::CXXConstructorDecl>(decl)) {
        return protocol::CompletionItemKind::Constructor;
    }

    if(llvm::isa<clang::CXXMethodDecl, clang::CXXConversionDecl, clang::CXXDestructorDecl>(decl)) {
        return protocol::CompletionItemKind::Method;
    }

    if(llvm::isa<clang::FunctionDecl>(decl)) {
        return protocol::CompletionItemKind::Function;
    }

    if(llvm::isa<clang::FieldDecl, clang::IndirectFieldDecl>(decl)) {
        return protocol::CompletionItemKind::Field;
    }

    if(llvm::isa<clang::VarDecl,
                 clang::ParmVarDecl,
                 clang::ImplicitParamDecl,
                 clang::BindingDecl,
                 clang::NonTypeTemplateParmDecl>(decl)) {
        return protocol::CompletionItemKind::Variable;
    }

    if(llvm::isa<clang::LabelDecl>(decl)) {
        return protocol::CompletionItemKind::Variable;
    }

    if(llvm::isa<clang::EnumDecl>(decl)) {
        return protocol::CompletionItemKind::Enum;
    }

    if(llvm::isa<clang::EnumConstantDecl>(decl)) {
        return protocol::CompletionItemKind::EnumMember;
    }

    if(llvm::isa<clang::RecordDecl,
                 clang::ClassTemplateDecl,
                 clang::ClassTemplateSpecializationDecl>(decl)) {
        return protocol::CompletionItemKind::Class;
    }

    if(llvm::isa<clang::TypedefNameDecl,
                 clang::TemplateTypeParmDecl,
                 clang::TemplateTemplateParmDecl,
                 clang::TypeAliasTemplateDecl,
                 clang::ConceptDecl>(decl)) {
        return protocol::CompletionItemKind::TypeParameter;
    }

    return protocol::CompletionItemKind::Text;
}

/// What a member access almost never names: the destructor, the class's
/// own name, an operator.
bool rarely_accessed(const clang::NamedDecl* decl) {
    if(llvm::isa<clang::CXXDestructorDecl>(decl)) {
        return true;
    }
    if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        return record->isInjectedClassName();
    }
    auto kind = decl->getDeclName().getNameKind();
    return kind == clang::DeclarationName::CXXOperatorName ||
           kind == clang::DeclarationName::CXXLiteralOperatorName ||
           kind == clang::DeclarationName::CXXConversionFunctionName;
}

/// The declaration an item shows of what Sema offers, whichever of its
/// declarations Sema met first, but never a friend's, which names the
/// parameters for itself and gives them no defaults: the newest one, with
/// the prototype and the defaults of those before it; a variable's first
/// one, as its later one is its out-of-line definition, its type spelled
/// through its class. Under a preamble's PCH, the friend an instantiation
/// of another class template declared before the class itself is the
/// first declaration of the class template.
const clang::NamedDecl* shown_declaration(const clang::NamedDecl* decl) {
    bool first = llvm::isa<clang::VarDecl, clang::VarTemplateDecl>(decl);
    const clang::Decl* shown = decl;
    for(const clang::Decl* each = decl->getMostRecentDecl(); each; each = each->getPreviousDecl()) {
        if(each->getFriendObjectKind() == clang::Decl::FOK_None) {
            shown = each;
            if(!first) {
                break;
            }
        }
    }
    return llvm::cast<clang::NamedDecl>(shown);
}

/// The policy Sema prints its completion strings with.
auto completion_policy(clang::Sema& sema) -> clang::PrintingPolicy {
    auto policy = sema.getPrintingPolicy();
    policy.AnonymousTagNameStyle =
        llvm::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
    policy.SuppressStrongLifetime = true;
    policy.SuppressUnwrittenScope = true;
    policy.CleanUglifiedParameters = true;
    return policy;
}

/// The completion string Sema builds for a class template, with the
/// template parameters of `pattern`: Sema reads them from the first
/// declaration, whichever it is.
const clang::CodeCompletionString&
    class_template_string(clang::CodeCompletionBuilder& builder,
                          const clang::CodeCompletionResult& candidate,
                          const clang::ClassTemplateDecl& pattern,
                          clang::Sema& sema) {
    auto policy = completion_policy(sema);
    auto copy = [&](llvm::StringRef text) {
        return builder.getAllocator().CopyString(text);
    };
    if(candidate.Qualifier) {
        std::string qualifier;
        llvm::raw_string_ostream stream(qualifier);
        candidate.Qualifier.print(stream, policy);
        if(candidate.QualifierIsInformative) {
            builder.AddInformativeChunk(copy(qualifier));
        } else {
            builder.AddTextChunk(copy(qualifier));
        }
    }
    builder.AddTypedTextChunk(copy(pattern.getName()));
    builder.AddChunk(clang::CodeCompletionString::CK_LeftAngle);
    // The parameters from the first with a default on are optional: no
    // part of the item shows them.
    bool first = true;
    for(const auto* param: *pattern.getTemplateParameters()) {
        std::string placeholder;
        auto named = [&](const clang::IdentifierInfo* name) {
            if(name) {
                placeholder += ' ';
                placeholder += name->deuglifiedName();
            }
        };
        if(const auto* type = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param)) {
            if(type->hasDefaultArgument()) {
                break;
            }
            if(type->wasDeclaredWithTypename()) {
                placeholder = "typename";
            } else if(const auto* constraint = type->getTypeConstraint()) {
                llvm::raw_string_ostream stream(placeholder);
                constraint->print(stream, policy);
            } else {
                placeholder = "class";
            }
            named(type->getIdentifier());
        } else if(const auto* value = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param)) {
            if(value->hasDefaultArgument()) {
                break;
            }
            if(const auto* name = value->getIdentifier()) {
                placeholder = name->deuglifiedName();
            }
            value->getType().getAsStringInternal(placeholder, policy);
        } else {
            const auto* nested = llvm::cast<clang::TemplateTemplateParmDecl>(param);
            if(nested->hasDefaultArgument()) {
                break;
            }
            placeholder = "template<...> class";
            named(nested->getIdentifier());
        }
        if(!first) {
            builder.AddChunk(clang::CodeCompletionString::CK_Comma);
        }
        first = false;
        builder.AddPlaceholderChunk(copy(placeholder));
    }
    builder.AddChunk(clang::CodeCompletionString::CK_RightAngle);
    return *builder.TakeString();
}

/// Whether a declaration of the tag's own name in its scope hides it: a
/// function, a variable or a typedef, as C headers write `struct stat`
/// beside `stat` and `typedef union attr attr;`. Sema hides the tag only
/// when it meets the other first, which depends on how its scope's lookup
/// lists them: a preamble's PCH lists them otherwise than a parse does.
bool hidden_tag(const clang::TagDecl* tag) {
    const auto* scope = tag->getDeclContext()->getRedeclContext();
    if(!tag->getDeclName() || !scope->isFileContext()) {
        return false;
    }
    return ranges::any_of(scope->lookup(tag->getDeclName()), [](const clang::NamedDecl* found) {
        return found->isInIdentifierNamespace(clang::Decl::IDNS_Ordinary);
    });
}

/// The place of each field a designator in an initializer of `record`
/// names, in declaration order; a field of an anonymous member by its own
/// name.
auto designator_order(const clang::RecordDecl* record) -> llvm::StringMap<std::uint32_t> {
    llvm::StringMap<std::uint32_t> order;
    for(auto* decl: record->decls()) {
        if(llvm::isa<clang::FieldDecl, clang::IndirectFieldDecl>(decl)) {
            if(auto name = llvm::cast<clang::NamedDecl>(decl)->getName(); !name.empty()) {
                order.try_emplace(name, order.size());
            }
        }
    }
    return order;
}

/// The most open access along the paths of bases from `derived` to `base`;
/// none when `base` is not one of them. A dependent base is followed to its
/// primary template, as Sema's lookup of visible members follows it.
auto base_access(const clang::CXXRecordDecl* derived, const clang::CXXRecordDecl* base)
    -> std::optional<clang::AccessSpecifier> {
    auto* target = base->getCanonicalDecl();
    auto reaches = [&](const clang::CXXBaseSpecifier* specifier, clang::CXXBasePath&) {
        auto type = specifier->getType();
        const clang::CXXRecordDecl* record = nullptr;
        if(!type->isDependentType()) {
            record = type->getAsCXXRecordDecl();
        } else if(auto* specialization = type->getAs<clang::TemplateSpecializationType>()) {
            if(auto* primary = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(
                   specialization->getTemplateName().getAsTemplateDecl())) {
                record = primary->getTemplatedDecl();
            }
        }
        return record && record->getCanonicalDecl() == target;
    };
    clang::CXXBasePaths paths;
    if(!derived->lookupInBases(reaches, paths, /*LookupInDependent=*/true)) {
        return std::nullopt;
    }
    return ranges::min_element(paths, {}, &clang::CXXBasePath::Access)->Access;
}

/// Whether `befriended`, the entity a friend declaration names, is `scope`.
/// A friend template a class template declares is linked to no declaration
/// until instantiated: like Sema's access check, one of the same kind, name
/// and scope counts, a function with as many parameters.
bool befriends(const clang::NamedDecl* befriended, const clang::NamedDecl* scope) {
    if(befriended->getCanonicalDecl() == scope->getCanonicalDecl()) {
        return true;
    }
    if(!llvm::isa<clang::RedeclarableTemplateDecl>(befriended) ||
       befriended->getKind() != scope->getKind() ||
       befriended->getDeclName() != scope->getDeclName() ||
       !befriended->getDeclContext()->getRedeclContext()->Equals(
           scope->getDeclContext()->getRedeclContext())) {
        return false;
    }
    auto* function = befriended->getAsFunction();
    return !function || function->getNumParams() == scope->getAsFunction()->getNumParams();
}

/// The dependent class a member access or a qualified name lists the
/// members of, when the completion point is outside it, the classes
/// derived from it and its friends. Sema cannot follow its dependent bases
/// to check access and checks a member found in one as if that base were
/// named: a member reached only through non-public bases would be offered.
auto unchecked_naming_class(clang::Sema& sema, clang::CodeCompletionContext& context)
    -> const clang::CXXRecordDecl* {
    using Kind = clang::CodeCompletionContext::Kind;
    const clang::CXXRecordDecl* naming = nullptr;
    if(context.getKind() == Kind::CCC_DotMemberAccess ||
       context.getKind() == Kind::CCC_ArrowMemberAccess) {
        naming = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
            clang::HeuristicResolver(sema.getASTContext())
                .resolveTypeToTagDecl(context.getBaseType()));
    } else if(auto spec = context.getCXXScopeSpecifier()) {
        naming = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
            sema.computeDeclContext(**spec, /*EnteringContext=*/true));
    }
    if(!naming || !naming->isDependentContext()) {
        return nullptr;
    }
    // The classes and functions around the completion point, walked as
    // Sema's access check walks them: a friend function defined in a class
    // is in that class. Each counts with its template too, which a friend
    // declaration may name instead.
    llvm::SmallVector<const clang::NamedDecl*, 8> scopes;
    for(const auto* scope = sema.CurContext; !scope->isFileContext();) {
        if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(scope)) {
            if(record->getCanonicalDecl() == naming->getCanonicalDecl() ||
               base_access(record, naming)) {
                return nullptr;
            }
            scopes.push_back(record);
            if(auto* pattern = record->getDescribedClassTemplate()) {
                scopes.push_back(pattern);
            } else if(auto* specialization =
                          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
                scopes.push_back(specialization->getSpecializedTemplate());
            }
            scope = record->getDeclContext();
        } else if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(scope)) {
            scopes.push_back(function);
            if(auto* pattern = function->getDescribedFunctionTemplate()) {
                scopes.push_back(pattern);
            }
            scope = function->getFriendObjectKind() ? function->getLexicalDeclContext()
                                                    : function->getDeclContext();
        } else {
            scope = scope->getParent();
        }
    }
    for(const auto* entry: naming->friends()) {
        const clang::NamedDecl* befriended = entry->getFriendDecl();
        if(auto* type = entry->getFriendType()) {
            befriended = type->getType()->getAsCXXRecordDecl();
        }
        if(befriended && ranges::any_of(scopes, [&](const clang::NamedDecl* scope) {
               return befriends(befriended, scope);
           })) {
            return nullptr;
        }
    }
    return naming;
}

/// The virtual functions of `record`'s bases, direct or not, an override
/// declaration in its body may name: none that it, or a class between it and
/// the base, overrides, and none final or deleted. Sema offers only its
/// direct bases'.
auto overridable(const clang::CXXRecordDecl& record)
    -> llvm::SmallVector<const clang::CXXMethodDecl*> {
    llvm::SmallVector<const clang::CXXRecordDecl*> bases;
    llvm::SmallPtrSet<const clang::CXXRecordDecl*, 8> seen;
    auto collect = [&](this auto& self, const clang::CXXRecordDecl& derived) -> void {
        for(const auto& base: derived.bases()) {
            const auto* decl = base.getType()->getAsCXXRecordDecl();
            decl = decl ? decl->getDefinition() : nullptr;
            if(decl && seen.insert(decl).second) {
                bases.push_back(decl);
                self(*decl);
            }
        }
    };
    collect(record);

    llvm::SmallPtrSet<const clang::CXXMethodDecl*, 16> overridden;
    auto close = [&](this auto& self, const clang::CXXMethodDecl& method) -> void {
        for(const auto* base: method.overridden_methods()) {
            if(overridden.insert(base->getCanonicalDecl()).second) {
                self(*base);
            }
        }
    };
    for(const auto* method: record.methods()) {
        close(*method);
    }
    for(const auto* base: bases) {
        for(const auto* method: base->methods()) {
            close(*method);
        }
    }

    // FIXME: a function final in one base branch is still offered from an
    // unrelated branch with the same name and signature (`D : L, R`, `L : A` with
    // `A::f() final`): accepting it overrides the final one, an error.
    llvm::SmallVector<const clang::CXXMethodDecl*> methods;
    for(const auto* base: bases) {
        for(const auto* method: base->methods()) {
            if(method->isVirtual() && method->getIdentifier() && !method->isDeleted() &&
               !method->hasAttr<clang::FinalAttr>() &&
               !overridden.contains(method->getCanonicalDecl())) {
                methods.push_back(method);
            }
        }
    }
    return methods;
}

/// The type a call of a variable holding a callable returns: a lambda's, a
/// function pointer's or reference's; null for any other variable.
clang::QualType call_result(const clang::NamedDecl* decl) {
    const auto* value = llvm::dyn_cast<clang::ValueDecl>(decl);
    if(!value) {
        return {};
    }
    auto type = value->getType().getNonReferenceType();
    if(auto* record = type->getAsCXXRecordDecl(); record && record->isLambda()) {
        return record->getLambdaCallOperator()->getReturnType();
    }
    if(type->isPointerType()) {
        type = type->getPointeeType();
    }
    if(const auto* function = type->getAs<clang::FunctionType>()) {
        return function->getReturnType();
    }
    return {};
}

/// Whether a value of type `from` can initialize one of type `to`, as far as
/// telling a call from its callee goes: the same type, or both arithmetic,
/// or both pointers to objects.
bool initializes(const clang::ASTContext& ast, clang::QualType from, clang::QualType to) {
    from = from.getNonReferenceType();
    to = to.getNonReferenceType();
    auto object_pointer = [](clang::QualType type) {
        return type->isPointerType() && !type->getPointeeType()->isFunctionType();
    };
    return ast.hasSameUnqualifiedType(from, to) ||
           (from->isArithmeticType() && to->isArithmeticType()) ||
           (object_pointer(from) && object_pointer(to));
}

/// Whether the context expects a function itself rather than the result of
/// calling it: a function pointer or reference, a member pointer.
bool expects_function(clang::QualType preferred) {
    if(preferred.isNull()) {
        return false;
    }
    preferred = preferred.getNonReferenceType();
    if(preferred->isMemberPointerType() || preferred->isFunctionType()) {
        return true;
    }
    return preferred->isPointerType() && preferred->getPointeeType()->isFunctionType();
}

/// Whether the completion names something unqualified, where the names of
/// other namespaces can be offered with the qualifier they need.
bool unqualified_name(clang::CodeCompletionContext& context) {
    using Kind = clang::CodeCompletionContext::Kind;
    switch(context.getKind()) {
        case Kind::CCC_TopLevel:
        case Kind::CCC_ClassStructUnion:
        case Kind::CCC_Statement:
        case Kind::CCC_Expression:
        case Kind::CCC_ParenthesizedExpression:
        case Kind::CCC_Type:
        case Kind::CCC_TopLevelOrExpression: return true;
        case Kind::CCC_Symbol:
        case Kind::CCC_SymbolOrNewName: {
            auto spec = context.getCXXScopeSpecifier();
            return !spec || (*spec)->isEmpty();
        }
        default: return false;
    }
}

/// Whether `offset` of `content` lies in a global module fragment: after a
/// leading `module;`, before the module declaration.
bool in_global_fragment(llvm::StringRef content,
                        std::uint32_t offset,
                        const clang::LangOptions& options) {
    Lexer lexer(content, options);
    bool fragment = false;
    for(auto token = lexer.advance(); !token.is_eof() && token.range.end <= offset;
        token = lexer.advance()) {
        if(token.is_eod()) {
            continue;
        }
        if(token.is_directive_hash()) {
            while(!token.is_eod() && !token.is_eof()) {
                token = lexer.advance();
            }
            continue;
        }
        auto text = token.text(content);
        if(text == "export") {
            token = lexer.advance();
            text = token.text(content);
        }
        if(text != "module" || lexer.advance().kind != clang::tok::semi) {
            return false;
        }
        fragment = true;
    }
    return fragment;
}

/// What a member access reaches without an object: a static member.
bool static_member(const clang::NamedDecl* decl) {
    if(const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(decl->getAsFunction())) {
        return method->isStatic();
    }
    return llvm::isa<clang::VarDecl>(decl);
}

/// In bundle mode several candidates may share one label: a class and its
/// constructors, or a declaration shadowed by a macro.
/// Label dedup keeps the highest-priority item. An active macro always
/// wins: the preprocessor rewrites the completed token, so a shadowed
/// declaration is not what the completion would invoke.
auto dedup_priority(protocol::CompletionItemKind kind, bool is_macro) -> int {
    if(is_macro) {
        return 4;
    }

    switch(kind) {
        case protocol::CompletionItemKind::Class: return 3;
        case protocol::CompletionItemKind::Function:
        case protocol::CompletionItemKind::Method: return 2;
        case protocol::CompletionItemKind::Constructor: return 1;
        default: return 0;
    }
}

/// Extract the function signature (parameter list) from a CodeCompletionString,
/// followed by the function's qualifiers when `qualifiers` is set. Returns
/// something like "(int x, float y = 1) const" for display in
/// labelDetails.detail; parameters with a default show it. `parameters`, when
/// given, receives where the parameter list opens, after the template
/// arguments a call must write (`<typename U>(U value)`); npos for none.
auto extract_signature(const clang::CodeCompletionString& ccs,
                       bool qualifiers,
                       std::size_t* parameters = nullptr) -> std::string {
    std::string signature;
    std::size_t opened = std::string::npos;
    bool in_parens = false;
    bool in_angles = false;
    bool closed = false;

    auto extract = [&](this auto& self, const clang::CodeCompletionString& chunks) -> void {
        for(const auto& chunk: chunks) {
            using CK = clang::CodeCompletionString::ChunkKind;
            switch(chunk.Kind) {
                case CK::CK_LeftParen:
                    in_parens = true;
                    opened = std::min(opened, signature.size());
                    signature += '(';
                    break;
                case CK::CK_RightParen:
                    signature += ')';
                    in_parens = false;
                    closed = true;
                    break;
                case CK::CK_Placeholder:
                case CK::CK_CurrentParameter:
                    if(in_parens && chunk.Text) {
                        signature += chunk.Text;
                    }
                    break;
                case CK::CK_Text:
                    if(in_parens && chunk.Text) {
                        signature += chunk.Text;
                    }
                    break;
                case CK::CK_Optional:
                    // A defaulted template parameter's placeholder leaves
                    // its default out: shown, it would read as required.
                    if(!in_angles) {
                        self(*chunk.Optional);
                    }
                    break;
                case CK::CK_Informative: {
                    // A function that cannot be called where completion
                    // happens (a destructor after `T::`) carries its whole
                    // parameter list as informative chunks, parentheses
                    // included. After the list come the function's
                    // qualifiers, ` const`.
                    llvm::StringRef text = chunk.Text ? chunk.Text : "";
                    if(text == "(") {
                        in_parens = true;
                        opened = std::min(opened, signature.size());
                    }
                    if(in_parens || (closed && qualifiers)) {
                        signature += text;
                    }
                    if(text == ")") {
                        in_parens = false;
                        closed = true;
                    }
                    break;
                }
                case CK::CK_LeftAngle:
                    signature += '<';
                    in_parens = true;
                    in_angles = true;
                    break;
                case CK::CK_RightAngle:
                    signature += '>';
                    in_parens = false;
                    in_angles = false;
                    break;
                case CK::CK_Comma:
                    if(in_parens) {
                        signature += ", ";
                    }
                    break;
                default: break;
            }
        }
    };
    extract(ccs);

    if(parameters) {
        *parameters = opened;
    }
    return signature;
}

/// Build a snippet string from a CodeCompletionString.
/// Produces e.g. "funcName(${1:int x}, ${2:float y})" for functions,
/// "ClassName<${1:T}>" for class templates, or a whole statement for a
/// keyword pattern. Empty when there is nothing to fill in.
auto build_snippet(const clang::CodeCompletionString& ccs) -> std::string {
    std::string snippet;
    unsigned placeholder_index = 0;

    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        switch(chunk.Kind) {
            case CK::CK_Placeholder:
                placeholder_index += 1;
                snippet += std::format("${{{}:{}}}",
                                       placeholder_index,
                                       escape_snippet(chunk.Text, /*placeholder=*/true));
                break;
            // Default arguments and display-only chunks are not inserted.
            case CK::CK_Optional:
            case CK::CK_Informative:
            case CK::CK_ResultType:
            case CK::CK_CurrentParameter: break;
            default: snippet += escape_snippet(chunk.Text); break;
        }
    }

    if(placeholder_index == 0) {
        return {};
    }

    return snippet;
}

/// A pattern's inserted text, whole or only through its typed text — the
/// part inserted without snippets: `if` for the if statement, the whole
/// declaration for an override.
auto pattern_text(const clang::CodeCompletionString& ccs, bool whole) -> std::string {
    std::string text;
    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        if(chunk.Kind == CK::CK_Optional || chunk.Kind == CK::CK_Informative ||
           chunk.Kind == CK::CK_ResultType) {
            continue;
        }
        text += chunk.Text;
        if(!whole && chunk.Kind == CK::CK_TypedText) {
            break;
        }
    }
    return text;
}

/// A pattern's text after its typed text, placeholders shown by name:
/// tells apart the two `for` statements.
auto pattern_tail(const clang::CodeCompletionString& ccs) -> std::string {
    std::string tail;
    bool typed = false;
    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        if(chunk.Kind == CK::CK_TypedText) {
            typed = true;
        } else if(typed && chunk.Kind != CK::CK_Optional && chunk.Kind != CK::CK_Informative) {
            tail += chunk.Kind == CK::CK_VerticalSpace ? " " : chunk.Text;
        }
    }
    return tail;
}

/// Extract the return type from a CodeCompletionString.
auto extract_return_type(const clang::CodeCompletionString& ccs) -> std::string {
    for(const auto& chunk: ccs) {
        if(chunk.Kind == clang::CodeCompletionString::CK_ResultType && chunk.Text) {
            return chunk.Text;
        }
    }
    return {};
}

/// One candidate as the item it becomes, before matching and bundling.
struct Candidate {
    std::string label;
    protocol::CompletionItemKind kind;

    /// What accepting inserts; the label when empty.
    std::string insert;
    bool snippet = false;

    /// What the typed prefix is matched against; the label when empty.
    std::string filter;

    /// Overload bundle the candidate joins; none when empty.
    std::string bundle;

    /// What accepting inserts when the candidate's bundle holds more than
    /// it: the call without placeholders. Empty when the candidate
    /// inserts no call.
    std::string bundled_insert;
    bool bundled_snippet = false;

    std::string signature;
    std::string return_type;
    bool deprecated = false;
    bool macro = false;

    /// The declaration whose comment documents the candidate.
    const clang::NamedDecl* documented = nullptr;

    /// Multiplies the score of the name's match: how strongly the context
    /// asks for the candidate.
    float relevance = 1.0F;

    /// Edits elsewhere in the text accepting also makes: the `.` of a
    /// member access through a pointer turned into `->`, an `import`.
    std::vector<TextReplacement> edits;

    /// The symbol whose header accepting must include (ScopeCandidate::symbol).
    std::uint64_t include = 0;

    /// Breaks ties in score: a field's place in a designated initializer.
    std::optional<std::uint32_t> order;
};

struct OverloadItem {
    protocol::CompletionItem item;
    float score = 0.0F;
    std::uint32_t count = 0;
    int dedup_priority = 0;
    std::string bundled_insert;
    bool bundled_snippet = false;
    const clang::NamedDecl* documented = nullptr;
};

struct CollectedItem {
    protocol::CompletionItem item;
    int dedup_priority = 0;
    const clang::NamedDecl* documented = nullptr;
};

/// Types of one compile spelled by names (TypePart), for another compile of
/// the same code to find again; false where a part names what no name
/// finds — a lambda, a local class.
struct Describer {
    explicit Describer(std::vector<TypePart>& out) : out(out) {}

    bool type(clang::QualType from) {
        auto canonical = from.getCanonicalType();
        const auto* bare = canonical.getTypePtr();
        auto index = out.size();
        auto& part = out.emplace_back();
        part.qualifiers = static_cast<std::uint8_t>(canonical.getLocalCVRQualifiers());
        auto made_of = [&](auto... types) {
            out[index].parts = sizeof...(types);
            return (type(types) && ...);
        };
        if(const auto* builtin = llvm::dyn_cast<clang::BuiltinType>(bare)) {
            part.kind = TypePart::Kind::Builtin;
            part.value = builtin->getKind();
            return true;
        }
        if(const auto* pointer = llvm::dyn_cast<clang::PointerType>(bare)) {
            part.kind = TypePart::Kind::Pointer;
            return made_of(pointer->getPointeeType());
        }
        if(const auto* reference = llvm::dyn_cast<clang::LValueReferenceType>(bare)) {
            part.kind = TypePart::Kind::LValueReference;
            return made_of(reference->getPointeeType());
        }
        if(const auto* reference = llvm::dyn_cast<clang::RValueReferenceType>(bare)) {
            part.kind = TypePart::Kind::RValueReference;
            return made_of(reference->getPointeeType());
        }
        if(const auto* array = llvm::dyn_cast<clang::ConstantArrayType>(bare)) {
            part.kind = TypePart::Kind::Array;
            part.value = static_cast<std::int64_t>(array->getSize().getZExtValue());
            return made_of(array->getElementType());
        }
        if(const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(bare)) {
            part.kind = TypePart::Kind::Function;
            part.variadic = function->isVariadic();
            part.nothrow = function->isNothrow();
            part.method_qualifiers = function->getMethodQuals().getCVRQualifiers();
            part.ref_qualifier = function->getRefQualifier();
            part.parts = function->getNumParams() + 1;
            return type(function->getReturnType()) &&
                   ranges::all_of(function->getParamTypes(),
                                  [&](clang::QualType parameter) { return type(parameter); });
        }
        if(const auto* tag = bare->getAsTagDecl()) {
            return this->tag(tag, index);
        }
        return false;
    }

    /// A class or an enumeration, a specialization by its template and
    /// arguments, a class template's own class by the template.
    bool tag(const clang::TagDecl* from, std::optional<std::size_t> index = std::nullopt) {
        if(!index) {
            index = out.size();
            out.emplace_back();
        }
        auto& part = out[*index];
        if(const auto* specialization =
               llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(from)) {
            if(llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(specialization)) {
                return false;
            }
            part.kind = TypePart::Kind::Specialization;
            auto arguments = specialization->getTemplateArgs().asArray();
            part.parts = arguments.size();
            return declarations(specialization->getSpecializedTemplate(), *index) &&
                   ranges::all_of(arguments, [&](const auto& each) { return argument(each); });
        }
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(from)) {
            if(const auto* pattern = record->getDescribedClassTemplate()) {
                part.kind = TypePart::Kind::Pattern;
                return declarations(pattern, *index);
            }
        }
        part.kind = TypePart::Kind::Tag;
        return declarations(from, *index);
    }

    bool argument(const clang::TemplateArgument& from) {
        switch(from.getKind()) {
            case clang::TemplateArgument::Type: return type(from.getAsType());
            case clang::TemplateArgument::Integral: {
                const auto& value = from.getAsIntegral();
                if(value.getSignificantBits() > 64) {
                    return false;
                }
                out.push_back({.kind = TypePart::Kind::Integral,
                               .value = value.isSigned()
                                            ? value.getSExtValue()
                                            : static_cast<std::int64_t>(value.getZExtValue()),
                               .parts = 1});
                return type(from.getIntegralType());
            }
            case clang::TemplateArgument::Template: {
                auto* named = from.getAsTemplate().getAsTemplateDecl();
                auto index = out.size();
                out.push_back({.kind = TypePart::Kind::Template});
                return named && declarations(named, index);
            }
            case clang::TemplateArgument::Pack: {
                out.push_back({.kind = TypePart::Kind::Pack,
                               .parts = static_cast<std::uint32_t>(from.pack_size())});
                return ranges::all_of(from.pack_elements(),
                                      [&](const auto& each) { return argument(each); });
            }
            default: return false;
        }
    }

    /// The declarations `from` is found through, down to itself.
    bool declarations(const clang::NamedDecl* from, std::size_t index) {
        llvm::SmallVector<TypePart::Declaration, 4> chain;
        const clang::Decl* decl = from;
        while(!llvm::isa<clang::TranslationUnitDecl>(decl)) {
            // A scope the name lookup passes through (an unscoped
            // enumeration, `extern "C"`) is no step of the way.
            const auto* context = llvm::dyn_cast<clang::DeclContext>(decl);
            if(decl == from || !context->isTransparentContext()) {
                const auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
                const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
                if(space && space->isAnonymousNamespace()) {
                    chain.push_back({.kind = decl->getKind()});
                } else if(named && named->getIdentifier()) {
                    chain.push_back({.name = named->getName().str(), .kind = decl->getKind()});
                } else {
                    return false;
                }
            }
            const auto* parent = decl->getDeclContext();
            if(!parent->isFileContext() && !parent->isRecord() && !parent->isTransparentContext()) {
                return false;
            }
            decl = llvm::cast<clang::Decl>(parent);
        }
        ranges::reverse(chain);
        out[index].declarations.assign(chain.begin(), chain.end());
        return true;
    }

    std::vector<TypePart>& out;
};

/// Types spelled by names (TypePart) rebuilt in this compile: each class
/// found again by the names it is found through, each specialization formed
/// again.
/// Null where a part names nothing found here — a declaration after the
/// completion point.
struct Rebuilder {
    Rebuilder(clang::Sema& sema, clang::SourceLocation loc, llvm::ArrayRef<TypePart> parts) :
        sema(sema), ast(sema.getASTContext()), loc(loc), parts(parts) {}

    bool done() const {
        return next == parts.size();
    }

    clang::QualType type() {
        assert(!done());
        const auto& part = parts[next];
        next += 1;
        clang::QualType result;
        switch(part.kind) {
            case TypePart::Kind::Builtin:
                result = builtin_type(static_cast<clang::BuiltinType::Kind>(part.value));
                break;
            case TypePart::Kind::Pointer:
                if(auto pointee = type(); !pointee.isNull()) {
                    result = ast.getPointerType(pointee);
                }
                break;
            case TypePart::Kind::LValueReference:
                if(auto pointee = type(); !pointee.isNull()) {
                    result = ast.getLValueReferenceType(pointee);
                }
                break;
            case TypePart::Kind::RValueReference:
                if(auto pointee = type(); !pointee.isNull()) {
                    result = ast.getRValueReferenceType(pointee);
                }
                break;
            case TypePart::Kind::Array:
                if(auto element = type(); !element.isNull()) {
                    result = ast.getConstantArrayType(
                        element,
                        llvm::APInt(64, static_cast<std::uint64_t>(part.value)),
                        nullptr,
                        clang::ArraySizeModifier::Normal,
                        0);
                }
                break;
            case TypePart::Kind::Function: {
                auto returned = type();
                llvm::SmallVector<clang::QualType, 4> parameters;
                for(std::uint32_t i = 1; i < part.parts; i += 1) {
                    parameters.push_back(type());
                }
                if(!returned.isNull() && ranges::none_of(parameters, &clang::QualType::isNull)) {
                    clang::FunctionProtoType::ExtProtoInfo info;
                    info.Variadic = part.variadic;
                    info.TypeQuals = clang::Qualifiers::fromCVRMask(part.method_qualifiers);
                    info.RefQualifier = static_cast<clang::RefQualifierKind>(part.ref_qualifier);
                    if(part.nothrow) {
                        info.ExceptionSpec.Type = clang::EST_BasicNoexcept;
                    }
                    result = ast.getFunctionType(returned, parameters, info);
                }
                break;
            }
            case TypePart::Kind::Tag:
            case TypePart::Kind::Pattern:
            case TypePart::Kind::Specialization: {
                next -= 1;
                auto* rebuilt = tag();
                result = rebuilt ? ast.getCanonicalTagType(rebuilt) : clang::QualType();
                break;
            }
            default: std::unreachable();
        }
        return result.isNull()
                   ? result
                   : ast.getQualifiedType(result, clang::Qualifiers::fromCVRMask(part.qualifiers));
    }

    /// The class here a part names: the class a specialization of it is
    /// formed of when it is concrete, the pattern of a class template for
    /// a dependent one.
    clang::TagDecl* tag() {
        assert(!done());
        const auto& part = parts[next];
        next += 1;
        switch(part.kind) {
            case TypePart::Kind::Tag: return llvm::dyn_cast_or_null<clang::TagDecl>(find(part));
            case TypePart::Kind::Pattern: {
                auto* primary = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(find(part));
                return primary ? primary->getTemplatedDecl() : nullptr;
            }
            case TypePart::Kind::Specialization: {
                auto* primary = llvm::dyn_cast_or_null<clang::ClassTemplateDecl>(find(part));
                /// As written: a parameter pack's arguments one by one.
                clang::TemplateArgumentListInfo written(loc, loc);
                bool complete = true;
                for(std::uint32_t i = 0; i < part.parts; i += 1) {
                    auto rebuilt = argument();
                    if(!rebuilt) {
                        complete = false;
                        continue;
                    }
                    auto add = [&](const clang::TemplateArgument& each) {
                        written.addArgument(sema.getTrivialTemplateArgumentLoc(each, {}, loc));
                    };
                    if(rebuilt->getKind() == clang::TemplateArgument::Pack) {
                        ranges::for_each(rebuilt->pack_elements(), add);
                    } else {
                        add(*rebuilt);
                    }
                }
                if(!primary || !complete) {
                    return nullptr;
                }
                auto type = sema.CheckTemplateIdType(clang::ElaboratedTypeKeyword::None,
                                                     clang::TemplateName(primary),
                                                     loc,
                                                     written,
                                                     /*Scope=*/nullptr,
                                                     /*ForNestedNameSpecifier=*/false);
                return type.isNull() ? nullptr : type->getAsTagDecl();
            }
            default: std::unreachable();
        }
    }

    std::optional<clang::TemplateArgument> argument() {
        assert(!done());
        const auto& part = parts[next];
        switch(part.kind) {
            case TypePart::Kind::Integral: {
                next += 1;
                auto rebuilt = type();
                if(rebuilt.isNull()) {
                    return std::nullopt;
                }
                bool is_unsigned = rebuilt->isUnsignedIntegerOrEnumerationType();
                llvm::APSInt value(llvm::APInt(ast.getIntWidth(rebuilt),
                                               static_cast<std::uint64_t>(part.value),
                                               !is_unsigned),
                                   is_unsigned);
                return clang::TemplateArgument(ast, value, rebuilt);
            }
            case TypePart::Kind::Template: {
                next += 1;
                auto* rebuilt = llvm::dyn_cast_or_null<clang::TemplateDecl>(find(part));
                if(!rebuilt) {
                    return std::nullopt;
                }
                return clang::TemplateArgument(clang::TemplateName(rebuilt));
            }
            case TypePart::Kind::Pack: {
                next += 1;
                llvm::SmallVector<clang::TemplateArgument, 4> elements;
                bool complete = true;
                for(std::uint32_t i = 0; i < part.parts; i += 1) {
                    if(auto rebuilt = argument()) {
                        elements.push_back(*rebuilt);
                    } else {
                        complete = false;
                    }
                }
                if(!complete) {
                    return std::nullopt;
                }
                return clang::TemplateArgument::CreatePackCopy(ast, elements);
            }
            default: {
                auto rebuilt = type();
                if(rebuilt.isNull()) {
                    return std::nullopt;
                }
                return clang::TemplateArgument(rebuilt);
            }
        }
    }

    clang::QualType builtin_type(clang::BuiltinType::Kind kind) {
        switch(kind) {
#define BUILTIN_TYPE(Id, SingletonId)                                                              \
    case clang::BuiltinType::Id: return ast.SingletonId;
#define PLACEHOLDER_TYPE(Id, SingletonId)
#define LAST_BUILTIN_TYPE(Id)
#include "clang/AST/BuiltinTypes.def"
            default: return {};
        }
    }

    /// The declaration here a part names, by the names and kinds of those
    /// it is found through.
    clang::NamedDecl* find(const TypePart& part) {
        clang::DeclContext* scope = ast.getTranslationUnitDecl();
        clang::NamedDecl* found = nullptr;
        for(const auto& each: part.declarations) {
            // All but the named one itself are namespaces and classes.
            if(found) {
                scope = llvm::cast<clang::DeclContext>(found);
            }
            found = nullptr;
            if(each.name.empty()) {
                if(auto* unit = llvm::dyn_cast<clang::TranslationUnitDecl>(scope)) {
                    found = unit->getAnonymousNamespace();
                } else if(auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
                    found = space->getAnonymousNamespace();
                }
            } else {
                for(auto* candidate: scope->lookup(&ast.Idents.get(each.name))) {
                    if(candidate->getKind() == each.kind) {
                        found = candidate;
                        break;
                    }
                }
            }
            if(!found) {
                return nullptr;
            }
        }
        return found;
    }

    clang::Sema& sema;
    clang::ASTContext& ast;
    clang::SourceLocation loc;
    llvm::ArrayRef<TypePart> parts;
    std::size_t next = 0;
};

/// The namespace a qualifier names at the point, through aliases; null for
/// another scope, whose names Sema reads whole regardless.
clang::DeclContext* qualified_namespace(clang::Sema& sema, const index::CompletionScope& scope) {
    auto& ast = sema.getASTContext();
    auto as_namespace = [](const clang::LookupResult& found) -> clang::DeclContext* {
        if(!found.isSingleResult()) {
            return nullptr;
        }
        auto* decl = found.getFoundDecl()->getUnderlyingDecl();
        if(auto* alias = llvm::dyn_cast<clang::NamespaceAliasDecl>(decl)) {
            return alias->getNamespace();
        }
        return llvm::dyn_cast<clang::NamespaceDecl>(decl);
    };
    llvm::ArrayRef<std::string> names = scope.qualifier;
    clang::DeclContext* space = ast.getTranslationUnitDecl();
    if(!scope.absolute) {
        clang::LookupResult found(sema,
                                  &ast.Idents.get(names.front()),
                                  {},
                                  clang::Sema::LookupNestedNameSpecifierName);
        found.suppressDiagnostics();
        sema.LookupName(found, sema.getCurScope());
        space = as_namespace(found);
        names = names.drop_front();
    }
    for(const auto& name: names) {
        if(!space) {
            return nullptr;
        }
        clang::LookupResult found(sema,
                                  &ast.Idents.get(name),
                                  {},
                                  clang::Sema::LookupNestedNameSpecifierName);
        found.suppressDiagnostics();
        sema.LookupQualifiedName(found, space);
        space = as_namespace(found);
    }
    return space;
}

/// Loads, of the precompiled preamble, what Sema lists at the point of
/// the candidates' names while it reads no other names of namespaces
/// (CompileCompletion::from_index): the declarations unqualified lookup
/// finds, or those of the namespace a qualifier names. A namespace the
/// index searched under another name, through an alias, is read whole.
/// Whether the qualifier's namespace brings in names of others, through a
/// using-directive or a using-declaration.
bool load_names(clang::Sema& sema,
                const index::CompletionScope& scope,
                llvm::ArrayRef<ScopeCandidate> candidates) {
    auto& ast = sema.getASTContext();
    clang::DeclContext* space = nullptr;
    bool brought = false;
    if(scope.qualified()) {
        space = qualified_namespace(sema, scope);
        if(!space) {
            return false;
        }
        llvm::SmallVector<llvm::StringRef, 4> names;
        for(auto* context = space; !context->isTranslationUnit(); context = context->getParent()) {
            if(auto* named = llvm::dyn_cast<clang::NamespaceDecl>(context);
               named && !named->isInline()) {
                names.push_back(named->getName());
            }
        }
        ranges::reverse(names);
        if(names.size() < scope.qualifier.size() ||
           !ranges::equal(llvm::ArrayRef(names).take_back(scope.qualifier.size()),
                          scope.qualifier,
                          {},
                          {},
                          [](const std::string& name) { return llvm::StringRef(name); })) {
            space->lookups();
            return false;
        }
        // A name of an inline or an anonymous namespace inside is the
        // namespace's own, as the index takes it.
        brought = llvm::any_of(space->using_directives(), [](clang::UsingDirectiveDecl* directive) {
            return !directive->getNominatedNamespace()->isAnonymousNamespace();
        });
    }
    auto own = [&](const clang::DeclContext* context) {
        for(context = context->getRedeclContext(); !context->Equals(space);
            context = context->getParent()) {
            auto* inner = llvm::dyn_cast<clang::NamespaceDecl>(context);
            if(!inner || (!inner->isInline() && !inner->isAnonymousNamespace())) {
                return false;
            }
        }
        return true;
    };
    for(const auto& candidate: candidates) {
        // Its identifier comes with the macro the PCH defines by its name.
        clang::LookupResult found(sema,
                                  &ast.Idents.get(candidate.name),
                                  {},
                                  clang::Sema::LookupOrdinaryName);
        found.suppressDiagnostics();
        if(space) {
            sema.LookupQualifiedName(found, space);
            brought |= llvm::any_of(found, [&](const clang::NamedDecl* decl) {
                return llvm::isa<clang::UsingShadowDecl>(decl) || !own(decl->getDeclContext());
            });
        } else if(!candidate.macro) {
            sema.LookupName(found, sema.getCurScope());
        }
    }
    return brought;
}

/// The function template `context` is in, through the lambdas and local
/// classes inside it.
const clang::FunctionDecl* enclosing_template(const clang::DeclContext* context) {
    for(; context; context = context->getParent()) {
        if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(context);
           function && function->getDescribedFunctionTemplate()) {
            return function;
        }
    }
    return nullptr;
}

class Builder final : public clang::CodeCompleteConsumer {
public:
    /// Sema reports macro candidates and statement keywords only on
    /// request: `if`, `for` and the like exist only as code patterns. The
    /// members the other access operator reaches come as candidates
    /// carrying the fix that writes it.
    /// With `from_index`, the names of namespaces the preamble declares
    /// are the candidates' alone (load_names).
    static clang::CodeCompleteOptions sema_options(const CodeCompletionOptions& options,
                                                   bool from_index) {
        clang::CodeCompleteOptions sema;
        sema.IncludeMacros = 1;
        sema.IncludeCodePatterns = 1;
        sema.IncludeFixIts = options.dot_to_arrow.value;
        sema.LoadExternal = !from_index;
        return sema;
    }

    Builder(CompilationUnit& unit,
            std::uint32_t offset,
            PositionEncoding encoding,
            std::vector<protocol::CompletionItem>& output,
            std::vector<const clang::NamedDecl*>& documented,
            const CodeCompletionOptions& options,
            const CompletionClient& client,
            const CompileCompletion& names,
            bool from_index,
            const InstantiatedTemplates& instantiated,
            bool& includes_pending,
            bool& member_access,
            bool& brought_in) :
        clang::CodeCompleteConsumer(sema_options(options, from_index)), unit(unit),
        focus(unit.focus_file()), offset(offset), encoding(encoding), output(output),
        documented(documented), options(options), argument_style(options.arguments), client(client),
        names(names), names_pending(from_index), instantiated(instantiated),
        includes_pending(includes_pending), member_access(member_access), brought_in(brought_in),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    clang::CodeCompletionAllocator& getAllocator() final {
        // Sema asks first thing in a completion, in the scope of the point.
        // The lexer reaches the point a token sooner: before the scope a `{`
        // opens or a declaration a `;` ends takes effect, and while the
        // parse tries skipping the body holding the point.
        if(names_pending) {
            names_pending = false;
            brought_in = load_names(unit.sema(), names.scope, names.candidates);
        }
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

    /// The entry `decl`, in the focus file, has among `entries`: at its
    /// name's offset, under its name.
    const static InstantiatedTemplates::Entry*
        recorded(clang::Sema& sema,
                 llvm::ArrayRef<InstantiatedTemplates::Entry> entries,
                 const clang::NamedDecl* decl) {
        auto& source_manager = sema.getSourceManager();
        auto [file, offset] =
            source_manager.getDecomposedLoc(source_manager.getFileLoc(decl->getLocation()));
        if(file != source_manager.getMainFileID()) {
            return nullptr;
        }
        auto name = decl->getNameAsString();
        auto found = ranges::find_if(entries, [&](const InstantiatedTemplates::Entry& entry) {
            return entry.offset == offset && entry.name == name;
        });
        return found == entries.end() ? nullptr : &*found;
    }

    /// The arguments `function`'s template takes in its only instantiation
    /// in the whole file, rebuilt here; none when it has no one
    /// instantiation there, or names what is not found here.
    llvm::SmallVector<clang::TemplateArgument, 4>
        instantiated_later(clang::Sema& sema, const clang::FunctionDecl* function) {
        const auto* entry = recorded(sema, instantiated.functions, function);
        if(!entry) {
            return {};
        }
        Rebuilder rebuilder(sema, sema.getPreprocessor().getCodeCompletionLoc(), entry->parts);
        llvm::SmallVector<clang::TemplateArgument, 4> arguments;
        while(!rebuilder.done()) {
            auto rebuilt = rebuilder.argument();
            if(!rebuilt) {
                return {};
            }
            arguments.push_back(*rebuilt);
        }
        return arguments;
    }

    /// The class `var`, the variable of a range-based `for`, holds, as the
    /// whole file tells: in the completion's own parse the statement is
    /// not finished, and its range is nowhere else. Null when the loop's
    /// element does not resolve there or names no class found here.
    clang::TagDecl* loop_class(clang::Sema& sema, const clang::VarDecl* var) {
        const auto* entry = recorded(sema, instantiated.loops, var);
        if(!entry) {
            return nullptr;
        }
        return Rebuilder(sema, sema.getPreprocessor().getCodeCompletionLoc(), entry->parts).tag();
    }

    /// Sema resolves a dependent member base or scope with its own
    /// heuristics, which stop at the primary template and a single typedef.
    /// When pseudo-instantiation reaches a class Sema did not, run the
    /// completion again against that class. Returns whether the nested run
    /// produced the reply, which is then in `output`.
    bool complete_resolved(clang::Sema& sema, clang::CodeCompletionContext& context) {
        using Kind = clang::CodeCompletionContext::Kind;
        auto kind = context.getKind();
        auto& ast = sema.getASTContext();
        auto* scope = sema.getCurScope();
        auto loc = sema.getPreprocessor().getCodeCompletionLoc();
        types::TemplateResolver resolver(ast);

        auto same = [](const clang::Decl* lhs, const clang::Decl* rhs) {
            return lhs && rhs && lhs->getCanonicalDecl() == rhs->getCanonicalDecl();
        };

        /// What a nested run may list the members of. A concrete
        /// specialization is instantiated first, as a written `X<int> x; x.`
        /// would be; one that does not instantiate, or a pattern without a
        /// definition, has nothing to list.
        auto definition = [&](clang::TagDecl* tag) -> clang::TagDecl* {
            if(!tag->isDependentContext() &&
               !sema.isCompleteType(loc, ast.getCanonicalTagType(tag))) {
                return nullptr;
            }
            return tag->getDefinition();
        };

        /// Sema's reply is replaced once the nested run offers candidates,
        /// even when the typed prefix then filters every one of them out; a
        /// nested run offering none keeps what an earlier callback left in
        /// `output`.
        auto run = [&](auto complete) {
            std::vector<protocol::CompletionItem> previous;
            std::vector<const clang::NamedDecl*> previous_documented;
            previous.swap(output);
            previous_documented.swap(documented);
            resolving = true;
            resolved_candidates = false;
            complete();
            resolving = false;
            if(!resolved_candidates) {
                output.swap(previous);
                documented.swap(previous_documented);
                return false;
            }
            return true;
        };

        if(kind == Kind::CCC_DotMemberAccess || kind == Kind::CCC_ArrowMemberAccess) {
            /// A designated initializer (`{ .§ }`) reports the same context
            /// as a member access, but without a base expression: its base
            /// type is the aggregate being initialized.
            auto* base = context.getBaseExpr();
            auto reported = context.getBaseType();
            bool arrow = kind == Kind::CCC_ArrowMemberAccess;
            auto object = reported;
            clang::TagDecl* tag = nullptr;
            if(base) {
                if(!base->isTypeDependent()) {
                    return false;
                }
                object = resolver.member_object(base, arrow);
                tag = object.isNull() ? nullptr : resolver.resolve_tag(object);
                /// The code after the completion point instantiates the
                /// template the completion is in, a generic lambda as the
                /// argument of a call above all.
                if(auto* function = enclosing_template(sema.CurContext); !tag && function) {
                    if(auto arguments = instantiated_later(sema, function); !arguments.empty()) {
                        types::TemplateResolver informed(
                            ast,
                            {.assumed = function, .assumed_arguments = std::move(arguments)});
                        object = informed.member_object(base, arrow);
                        tag = object.isNull() ? nullptr : informed.resolve_tag(object);
                    }
                }
                /// The members of a dependent specialization read its
                /// template's parameters as its arguments.
                display_options.specialization =
                    object.isNull() ? nullptr : object->getAs<clang::TemplateSpecializationType>();
                if(auto* name = llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
                   !tag && !arrow && name) {
                    if(auto* var = llvm::dyn_cast<clang::VarDecl>(name->getDecl());
                       var && var->isCXXForRangeDecl()) {
                        tag = loop_class(sema, var);
                        object = tag ? ast.getCanonicalTagType(tag) : clang::QualType();
                    }
                }
            } else if(!reported->isDependentType()) {
                return false;
            } else {
                tag = resolver.resolve_tag(object);
            }
            if(!tag || same(tag, clang::HeuristicResolver(ast).resolveTypeToTagDecl(reported))) {
                return false;
            }
            auto* record = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(definition(tag));
            if(!record) {
                return false;
            }
            auto type =
                ast.getQualifiedType(ast.getCanonicalTagType(record), object.getQualifiers());

            if(!base) {
                /// A class without a named field offers no designator; the
                /// resolved reply is empty, not what Sema found elsewhere.
                bool named = ranges::any_of(record->decls(), [](const clang::Decl* decl) {
                    return llvm::isa<clang::FieldDecl, clang::IndirectFieldDecl>(decl) &&
                           llvm::cast<clang::NamedDecl>(decl)->getDeclName();
                });
                if(!named) {
                    output.clear();
                    documented.clear();
                    return true;
                }
                return run([&] {
                    sema.CodeCompletion().CodeCompleteDesignator(type, {}, clang::Designation());
                });
            }

            auto* stand_in = new (ast) clang::OpaqueValueExpr(loc, type, clang::VK_LValue);
            return run([&] {
                sema.CodeCompletion().CodeCompleteMemberReferenceExpr(scope,
                                                                      stand_in,
                                                                      /*OtherOpBase=*/nullptr,
                                                                      loc,
                                                                      /*IsArrow=*/false,
                                                                      /*IsBaseExprStatement=*/false,
                                                                      context.getPreferredType());
            });
        }

        if(kind == Kind::CCC_Symbol) {
            auto spec = context.getCXXScopeSpecifier();
            if(!spec) {
                return false;
            }
            auto NNS = (*spec)->getScopeRep();
            if(NNS.getKind() != clang::NestedNameSpecifier::Kind::Type || !NNS.isDependent()) {
                return false;
            }
            auto scope_type = clang::QualType(NNS.getAsType(), 0);
            auto* tag = resolver.resolve_tag(scope_type);
            if(auto* function = enclosing_template(sema.CurContext); !tag && function) {
                if(auto arguments = instantiated_later(sema, function); !arguments.empty()) {
                    types::TemplateResolver informed(
                        ast,
                        {.assumed = function, .assumed_arguments = std::move(arguments)});
                    tag = informed.resolve_tag(scope_type);
                }
            }
            auto* entered = llvm::dyn_cast_or_null<clang::TagDecl>(
                sema.computeDeclContext(**spec, /*EnteringContext=*/true));
            if(!tag || same(tag, entered)) {
                return false;
            }
            tag = definition(tag);
            if(!tag) {
                return false;
            }

            /// Sema computes no declaration context for an enumeration
            /// inside a template; offer its enumerators directly.
            if(auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(tag)) {
                std::vector<clang::CodeCompletionResult> enumerators;
                for(auto* enumerator: enumeration->enumerators()) {
                    enumerators.emplace_back(enumerator, clang::CCP_Constant);
                }
                return run([&] {
                    ProcessCodeCompleteResults(sema,
                                               context,
                                               enumerators.data(),
                                               enumerators.size());
                });
            }

            clang::CXXScopeSpec resolved;
            resolved.MakeTrivial(
                ast,
                clang::NestedNameSpecifier(ast.getCanonicalTagType(tag).getTypePtr()),
                (*spec)->getRange());
            return run([&] {
                sema.CodeCompletion().CodeCompleteQualifiedId(scope,
                                                              resolved,
                                                              /*EnteringContext=*/false,
                                                              context.isUsingDeclaration(),
                                                              /*IsAddressOfOperand=*/false,
                                                              /*IsInDeclarationContext=*/false,
                                                              /*BaseType=*/{},
                                                              context.getPreferredType());
            });
        }

        return false;
    }

    void ProcessCodeCompleteResults(clang::Sema& sema,
                                    clang::CodeCompletionContext context,
                                    clang::CodeCompletionResult* candidates,
                                    unsigned candidate_count) final {
        // In a conditional block the build turns off, clang lexes raw and
        // offers the names of whatever scope the parser stood in when its
        // lookahead reached the block: the scope a preamble's PCH leaves
        // differs from the one a whole parse does.
        auto* lexer = sema.getPreprocessor().getCurrentFileLexer();
        if(context.getKind() == clang::CodeCompletionContext::CCC_Recovery ||
           (lexer && lexer->isLexingRawMode())) {
            return;
        }

        if(resolving) {
            resolved_candidates |= candidate_count > 0;
        } else {
            member_access =
                context.getKind() == clang::CodeCompletionContext::CCC_DotMemberAccess ||
                context.getKind() == clang::CodeCompletionContext::CCC_ArrowMemberAccess;
            display_options.specialization = nullptr;
            if(complete_resolved(sema, context)) {
                return;
            }
        }

        if(candidate_count == 0) {
            return;
        }

        auto& source_manager = sema.getSourceManager();
        auto content = focus.content();
        auto prefix = Prefix::from(content, offset);
        FuzzyMatcher matcher(prefix.spelling);

        auto map = focus.positions();
        auto typed = map.range(prefix.typed, encoding);
        auto whole = map.range(prefix.whole, encoding);
        if(!typed || !whole) {
            return;
        }
        // Call or template arguments already written after the name.
        auto after = content.drop_front(prefix.whole.end).ltrim();
        bool arguments_follow = after.starts_with("(") || after.starts_with("<");

        std::vector<CollectedItem> collected;
        collected.reserve(candidate_count);

        std::vector<OverloadItem> overloads;
        overloads.reserve(candidate_count);
        std::unordered_map<std::string, std::size_t> overload_index;

        auto match = [&](llvm::StringRef filter) -> std::optional<float> {
            // Filter out _/__ prefixed internal symbols unless user typed _.
            if(filter.empty() || (filter.starts_with("_") && !prefix.spelling.starts_with("_"))) {
                return std::nullopt;
            }
            return matcher.match(filter);
        };

        auto build_item = [&](const Candidate& candidate, float score) {
            protocol::CompletionItem item{
                .label = candidate.label,
            };
            item.kind = candidate.kind;
            auto text = candidate.insert.empty() ? candidate.label : candidate.insert;
            // A statement spanning lines follows the indentation it lands at.
            if(text.find('\n') != std::string::npos) {
                item.insert_text_mode = protocol::InsertTextMode::AdjustIndentation;
            }
            if(client.insert_replace && prefix.whole != prefix.typed) {
                item.text_edit = protocol::InsertReplaceEdit{
                    .new_text = std::move(text),
                    .insert = *typed,
                    .replace = *whole,
                };
            } else {
                item.text_edit = protocol::TextEdit{
                    .range = *whole,
                    .new_text = std::move(text),
                };
            }
            if(candidate.snippet) {
                item.insert_text_format = protocol::InsertTextFormat::Snippet;
            }
            if(!candidate.filter.empty()) {
                item.filter_text = candidate.filter;
            }
            item.sort_text = sort_text(score, candidate.order);
            if(!candidate.edits.empty()) {
                std::vector<protocol::TextEdit> edits;
                for(const auto& edit: candidate.edits) {
                    if(auto range = map.range(edit.range, encoding)) {
                        edits.push_back({.range = *range, .new_text = edit.text});
                    }
                }
                item.additional_text_edits = std::move(edits);
            }
            if(candidate.include != 0) {
                item.data = protocol::LSPAny(std::format("{}", candidate.include));
                includes_pending = true;
            }
            if(!candidate.signature.empty() || !candidate.return_type.empty()) {
                protocol::CompletionItemLabelDetails details;
                if(!candidate.signature.empty()) {
                    details.detail = candidate.signature;
                }
                if(!candidate.return_type.empty()) {
                    details.description = candidate.return_type;
                }
                item.label_details = std::move(details);
            }
            if(candidate.deprecated) {
                item.tags = std::vector{protocol::CompletionItemTag::Deprecated};
            }
            return item;
        };

        auto add = [&](const Candidate& candidate) {
            auto matched = match(candidate.filter.empty() ? candidate.label : candidate.filter);
            if(!matched) {
                return;
            }
            auto score = *matched * candidate.relevance;
            if(candidate.deprecated) {
                score *= 0.5F;
            }

            // A name the file does not reach yet yields to one it does.
            int priority =
                candidate.include != 0 ? -1 : dedup_priority(candidate.kind, candidate.macro);

            if(!candidate.bundle.empty()) {
                auto [it, inserted] =
                    overload_index.try_emplace(candidate.bundle, overloads.size());
                if(inserted) {
                    overloads.push_back({
                        .item = build_item(candidate, score),
                        .score = score,
                        .count = 1,
                        .dedup_priority = priority,
                        .bundled_insert = candidate.bundled_insert,
                        .bundled_snippet = candidate.bundled_snippet,
                        .documented = candidate.documented,
                    });
                } else {
                    auto& existing = overloads[it->second];
                    existing.count += 1;
                    if(score > existing.score) {
                        existing.score = score;
                        existing.item.sort_text = sort_text(score, std::nullopt);
                    }
                }
                return;
            }

            collected.push_back({.item = build_item(candidate, score),
                                 .dedup_priority = priority,
                                 .documented = candidate.documented});
        };

        bool keyword_snippets = options.statement_snippets && client.snippets;
        llvm::StringSet<> plain_patterns;
        auto add_pattern = [&](const clang::CodeCompletionString& pattern, unsigned priority) {
            auto label = pattern.getAllTypedText();
            auto head = pattern_text(pattern, /*whole=*/false);
            if(keyword_snippets) {
                auto snippet = build_snippet(pattern);
                bool has_snippet = !snippet.empty();
                add({
                    .label = label,
                    .kind = protocol::CompletionItemKind::Snippet,
                    .insert =
                        has_snippet ? std::move(snippet) : pattern_text(pattern, /*whole=*/true),
                    .snippet = has_snippet,
                    .signature = pattern_tail(pattern),
                    .relevance = priority_weight(priority),
                });
            } else if(plain_patterns.insert(head).second) {
                // Without its placeholders a pattern is its keyword;
                // the variants of one statement collapse.
                add({
                    .label = label,
                    .kind = protocol::CompletionItemKind::Keyword,
                    .insert = head,
                    .relevance = priority_weight(priority),
                });
            }
        };

        // In a module unit, imports precede every other declaration of its
        // purview, none in its global module fragment, and only an interface
        // exports; elsewhere `export` only begins one, before any
        // declaration. Clang's keywords know neither.
        auto* module = sema.getCurrentModule();
        bool named_module = module && module->isNamedModule();
        auto completion_loc = sema.getPreprocessor().getCodeCompletionLoc();
        auto declared = ranges::any_of(
            sema.getASTContext().getTranslationUnitDecl()->noload_decls(),
            [&](const clang::Decl* decl) {
                return !decl->isImplicit() && !llvm::isa<clang::ImportDecl>(decl) &&
                       (named_module ? decl->isInNamedModule()
                                     : source_manager.isWrittenInMainFile(decl->getLocation())) &&
                       source_manager.isBeforeInTranslationUnit(decl->getLocation(),
                                                                completion_loc);
            });
        bool imports = named_module
                           ? !declared
                           : !in_global_fragment(content, prefix.typed.begin, sema.getLangOpts());
        bool exports = named_module ? module->isModuleInterfaceUnit() : !declared;
        auto allowed_keyword = [&](llvm::StringRef keyword) {
            return (keyword != "import" || imports) && (keyword != "export" || exports);
        };

        bool member_access =
            context.getKind() == clang::CodeCompletionContext::CCC_DotMemberAccess ||
            context.getKind() == clang::CodeCompletionContext::CCC_ArrowMemberAccess;
        // What the system headers declare ranks after the rest where it
        // competes with it: not among the members of a class or a scope.
        bool unqualified = unqualified_name(context);
        types::TemplateResolver resolver(sema.getASTContext());
        auto* naming = unchecked_naming_class(sema, context);
        // In `{ .` the fields rank in declaration order, the order C++
        // designates them in: there those before the last one written
        // cannot follow. A field designated as a whole cannot be again; one
        // designated in part (`.inner.x = 1`) can (`.inner.y = 2`).
        llvm::StringMap<std::uint32_t> field_order;
        llvm::StringSet<> designated;
        if(member_access && !context.getBaseExpr()) {
            if(auto* record = llvm::dyn_cast_or_null<clang::RecordDecl>(
                   clang::HeuristicResolver(sema.getASTContext())
                       .resolveTypeToTagDecl(context.getBaseType()))) {
                field_order = designator_order(record);
            }
            std::uint32_t passed = 0;
            for(auto designator:
                written_designators(content, prefix.typed.begin, sema.getLangOpts())) {
                if(designator.whole) {
                    designated.insert(designator.field);
                }
                if(auto it = field_order.find(designator.field);
                   sema.getLangOpts().CPlusPlus && it != field_order.end()) {
                    passed = std::max(passed, it->second);
                }
            }
            for(auto& [name, order]: field_order) {
                if(order < passed) {
                    designated.insert(name);
                }
            }
        }

        auto left_out = [&](const clang::CodeCompletionResult& candidate) {
            auto* declaration = candidate.Declaration;
            // A hidden declaration is not what its name means here.
            if(!declaration || candidate.Hidden ||
               candidate.Availability == CXAvailability_NotAccessible) {
                return true;
            }
            // The members `.` reaches on an object with an `operator->`
            // are not what its `->` asks for.
            if(!candidate.FixIts.empty() &&
               context.getKind() == clang::CodeCompletionContext::CCC_ArrowMemberAccess) {
                return true;
            }
            // Clang gives a deduction guide no name to type.
            if(declaration->getDeclName().getNameKind() ==
               clang::DeclarationName::CXXDeductionGuideName) {
                return true;
            }
            if(member_access && rarely_accessed(declaration)) {
                return true;
            }
            if(auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration);
               field && designated.contains(field->getName())) {
                return true;
            }
            // A using-declaration is named where it is, not where its target is.
            const clang::NamedDecl* found = candidate.ShadowDecl;
            if(!found) {
                found = declaration;
            }
            auto* owner = llvm::dyn_cast<clang::CXXRecordDecl>(found->getDeclContext());
            if(!naming || !owner) {
                return false;
            }
            auto access = base_access(naming, owner);
            return access && *access != clang::AS_public;
        };

        /// What accepting a call inserts after the name, unless the
        /// arguments are written already or the name is not called here:
        /// its placeholders, or its parentheses with the cursor between them
        /// — what a bundle of several overloads takes too. A call without
        /// parameters is complete with its parentheses.
        auto insert_call = [&](Candidate& item,
                               const clang::CodeCompletionString& ccs,
                               bool called_here) {
            if(arguments_follow || !called_here || argument_style == CompletionArguments::None) {
                return;
            }
            auto complete = item.label + "()";
            if(!client.snippets) {
                item.insert = complete;
                item.bundled_insert = std::move(complete);
                return;
            }
            item.bundled_insert = escape_snippet(item.label) + "($0)";
            item.bundled_snippet = true;
            auto snippet = build_snippet(ccs);
            if(snippet.empty()) {
                item.insert = std::move(complete);
            } else if(argument_style == CompletionArguments::Placeholders) {
                item.insert = std::move(snippet);
                item.snippet = true;
            } else {
                item.insert = item.bundled_insert;
                item.snippet = true;
            }
        };

        /// After `t.`, `t->` or `T::` on a template parameter, Sema offers
        /// what the constraints on it require as patterns: a name and, for
        /// a call, the types of the arguments it is called with.
        bool requirements = false;
        if(member_access) {
            requirements = llvm::isa<clang::TemplateTypeParmType>(context.getBaseType());
        } else if(auto spec = context.getCXXScopeSpecifier()) {
            auto scope = (*spec)->getScopeRep();
            requirements = scope.getKind() == clang::NestedNameSpecifier::Kind::Type &&
                           llvm::isa<clang::TemplateTypeParmType>(scope.getAsType());
        }
        auto add_requirement = [&](const clang::CodeCompletionString& pattern, unsigned priority) {
            Candidate item{
                .label = pattern.getAllTypedText(),
                .signature = extract_signature(pattern, /*qualifiers=*/false),
                .return_type = extract_return_type(pattern),
            };
            if(item.signature.empty()) {
                // After `T::` a name without a call is mostly a type the
                // constraints name, `typename T::value_type`.
                item.kind = member_access ? protocol::CompletionItemKind::Field
                                          : protocol::CompletionItemKind::TypeParameter;
            } else {
                item.kind = protocol::CompletionItemKind::Method;
                insert_call(item, pattern, /*called_here=*/true);
            }
            item.relevance = priority_weight(priority);
            add(item);
        };

        /// Whether `decl` is, or a call of it returns, the type the context
        /// expects.
        auto expected = [&, preferred = context.getPreferredType()](const clang::NamedDecl* decl) {
            if(preferred.isNull()) {
                return false;
            }
            clang::QualType type;
            if(auto* value = llvm::dyn_cast<clang::ValueDecl>(decl)) {
                type = value->getType();
            } else if(auto* function = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
                type = function->getTemplatedDecl()->getType();
            }
            if(auto* function = type.isNull() ? nullptr : type->getAs<clang::FunctionType>()) {
                type = function->getReturnType();
            }
            return !type.isNull() &&
                   sema.getASTContext().hasSameUnqualifiedType(type.getNonReferenceType(),
                                                               preferred.getNonReferenceType());
        };

        llvm::DenseSet<const clang::Decl*> offered;
        /// After `struct`, `union` or `enum`, where Sema offers only tags.
        bool tags_only =
            ranges::contains(std::array{clang::CodeCompletionContext::CCC_ClassOrStructTag,
                                        clang::CodeCompletionContext::CCC_UnionTag,
                                        clang::CodeCompletionContext::CCC_EnumTag},
                             context.getKind());
        /// `relevance` scales what the context makes of the candidate.
        auto add_declaration = [&](clang::CodeCompletionResult& candidate, float relevance) {
            if(auto* tag = llvm::dyn_cast<clang::TagDecl>(candidate.Declaration);
               tag && !tags_only && hidden_tag(tag)) {
                return;
            }
            candidate.Declaration = shown_declaration(candidate.Declaration);
            auto* declaration = candidate.Declaration;
            offered.insert(declaration->getCanonicalDecl());
            auto name = display::name_of(declaration,
                                         {
                                             .suppress_ctor_template_args = true,
                                             .qualified = false,
                                         });
            // Sema offers every name in scope, of which the typed prefix
            // keeps a few: what follows is too costly to spend on the rest.
            if(!match(name)) {
                return;
            }

            // A qualifier clang does not mark informative must be
            // written: the name alone does not reach the
            // declaration (a scoped enumerator outside its enum).
            std::string qualifier;
            if(candidate.Qualifier && !candidate.QualifierIsInformative) {
                llvm::raw_string_ostream stream(qualifier);
                candidate.Qualifier.print(stream, sema.getPrintingPolicy());
            }

            auto kind = completion_kind(declaration);
            Candidate item{
                .label = qualifier + name,
                .kind = kind,
                .filter = qualifier.empty() ? std::string() : name,
                .deprecated = candidate.Availability == CXAvailability_Deprecated,
                .documented = declaration,
            };

            // Clang offers a namespace last, among the qualifiers; but one is
            // typed to name what it declares, like any other declaration.
            bool space = llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(declaration);
            auto priority =
                space ? std::min(candidate.Priority, static_cast<unsigned>(clang::CCP_Declaration))
                      : candidate.Priority;
            // Clang weighs every name by the type expected but a member's.
            if(member_access && expected(declaration)) {
                priority /= clang::CCF_ExactTypeMatch;
            }
            item.relevance = relevance * priority_weight(priority);
            if(unqualified && source_manager.isInSystemHeader(declaration->getLocation())) {
                item.relevance *= 0.5F;
            }
            if(member_access && static_member(declaration)) {
                item.relevance *= 0.5F;
            }
            for(const auto& fix: candidate.FixIts) {
                auto range = clang::Lexer::makeFileCharRange(fix.RemoveRange,
                                                             source_manager,
                                                             sema.getLangOpts());
                auto begin = focus.offset(range.getBegin());
                auto end = focus.offset(range.getEnd());
                if(begin && end) {
                    item.edits.push_back({
                        .range = {*begin, *end},
                        .text = fix.CodeToInsert
                    });
                }
            }
            if(!item.edits.empty()) {
                item.relevance *= 0.5F;
            }

            if(auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration)) {
                if(auto it = field_order.find(field->getName()); it != field_order.end()) {
                    item.order = it->second;
                }
            }

            bool is_callable = kind == protocol::CompletionItemKind::Function ||
                               kind == protocol::CompletionItemKind::Method ||
                               kind == protocol::CompletionItemKind::Constructor;
            if(options.bundle_overloads && is_callable) {
                item.bundle = item.label;
            }

            clang::CodeCompletionBuilder builder(getAllocator(), getCodeCompletionTUInfo());
            const auto* pattern = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration);
            auto& ccs = pattern && !candidate.StartsNestedNameSpecifier
                            ? class_template_string(builder, candidate, *pattern, sema)
                            : *candidate.CreateCodeCompletionString(sema,
                                                                    context,
                                                                    getAllocator(),
                                                                    getCodeCompletionTUInfo(),
                                                                    /*IncludeBriefComments=*/false);
            // Clang gives a variable holding a lambda the signature of its
            // call operator, whose `const` is no part of calling it.
            bool method = llvm::isa_and_present<clang::CXXMethodDecl>(declaration->getAsFunction());
            std::size_t parameters;
            item.signature = extract_signature(ccs, /*qualifiers=*/method, &parameters);
            item.return_type = extract_return_type(ccs);
            // Where Sema gives a parameter list and a type, they are printed
            // as every feature prints them.
            if(!readable) {
                readable.emplace(sema.getASTContext(), display_options);
                display_options.readable = &*readable;
            }
            display_options.scope = declaration->getDeclContext();
            const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
            display_options.source = variable ? variable->getInit() : nullptr;
            const auto* function = declaration->getAsFunction();
            if(function && parameters != std::string::npos) {
                item.signature.replace(parameters,
                                       std::string::npos,
                                       display::call_signature(*function, display_options));
            }
            if(!item.return_type.empty()) {
                clang::QualType type;
                if(function) {
                    type = function->getReturnType();
                } else if(item.signature.starts_with("(")) {
                    // A variable holding something called: what the call gives.
                    type = call_result(declaration);
                } else if(const auto* enumerator =
                              llvm::dyn_cast<clang::EnumConstantDecl>(declaration)) {
                    // C types an enumerator `int`, as C++ does inside its
                    // enum: its enum is what it reads as.
                    type = sema.getASTContext().getCanonicalTagType(
                        llvm::cast<clang::EnumDecl>(enumerator->getDeclContext()));
                } else if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(declaration)) {
                    type = value->getType();
                }
                if(!type.isNull()) {
                    item.return_type =
                        display::type(sema.getASTContext(), type, display_options).text;
                }
            }
            auto preferred = context.getPreferredType();
            if(is_callable) {
                insert_call(item,
                            ccs,
                            kind != protocol::CompletionItemKind::Constructor &&
                                candidate.FunctionCanBeCall && !candidate.DeclaringEntity &&
                                !context.isUsingDeclaration() && !expects_function(preferred));
            } else if(item.signature.starts_with("(")) {
                // A variable holding a lambda is mostly passed on: it is
                // called where its call gives what the context expects.
                auto result = call_result(declaration);
                insert_call(item,
                            ccs,
                            preferred.isNull() ||
                                (!result.isNull() && !preferred->isDependentType() &&
                                 initializes(sema.getASTContext(), result, preferred)));
            } else if(argument_style != CompletionArguments::None && client.snippets &&
                      !arguments_follow && !context.isUsingDeclaration() &&
                      llvm::isa<clang::ClassTemplateDecl,
                                clang::TypeAliasTemplateDecl,
                                clang::VarTemplateDecl>(declaration)) {
                auto snippet = build_snippet(ccs);
                // Every parameter defaulted: nothing to fill in, but the
                // name alone does not name a type.
                if(snippet.empty()) {
                    item.insert = item.label + "<>";
                } else if(argument_style == CompletionArguments::Placeholders) {
                    item.insert = std::move(snippet);
                    item.snippet = true;
                } else {
                    item.insert = escape_snippet(item.label) + "<$0>";
                    item.snippet = true;
                }
            }

            add(item);
        };

        for(auto& candidate: ranges::subrange(candidates, candidates + candidate_count)) {
            switch(candidate.Kind) {
                case clang::CodeCompletionResult::RK_Keyword:
                    if(allowed_keyword(candidate.Keyword)) {
                        add({.label = candidate.Keyword,
                             .kind = protocol::CompletionItemKind::Keyword,
                             .relevance = priority_weight(candidate.Priority)});
                    }
                    break;

                case clang::CodeCompletionResult::RK_Pattern: {
                    auto& pattern = *candidate.Pattern;
                    // A pattern carrying a method is an override declaration
                    // in a class body, which the overrides below offer for
                    // every base, or in a method body a call of the
                    // overridden method — a duplicate of the method's own
                    // candidate that would change which function is called.
                    if(candidate.Declaration) {
                        break;
                    }
                    if(requirements) {
                        add_requirement(pattern, candidate.Priority);
                        break;
                    }
                    if(allowed_keyword(pattern.getTypedText())) {
                        add_pattern(pattern, candidate.Priority);
                    }
                    break;
                }

                case clang::CodeCompletionResult::RK_Macro: {
                    if(!match(candidate.Macro->getName())) {
                        break;
                    }
                    auto* info = sema.getPreprocessor().getMacroInfo(candidate.Macro);
                    bool function_like = info && info->isFunctionLike();

                    Candidate item{
                        .label = candidate.Macro->getName().str(),
                        .kind = function_like ? protocol::CompletionItemKind::Function
                                              : protocol::CompletionItemKind::Constant,
                        .macro = true,
                        .relevance = priority_weight(candidate.Priority),
                    };
                    if(unqualified && info &&
                       source_manager.isInSystemHeader(info->getDefinitionLoc())) {
                        item.relevance *= 0.5F;
                    }
                    if(auto* ccs =
                           candidate.CreateCodeCompletionString(sema,
                                                                context,
                                                                getAllocator(),
                                                                getCodeCompletionTUInfo(),
                                                                /*IncludeBriefComments=*/false)) {
                        item.signature = extract_signature(*ccs, /*qualifiers=*/false);
                        if(function_like) {
                            insert_call(item, *ccs, /*called_here=*/true);
                        }
                    }
                    add(item);
                    break;
                }

                case clang::CodeCompletionResult::RK_Declaration: {
                    if(left_out(candidate)) {
                        break;
                    }
                    // Sema leaves a using-declaration naming a member of a
                    // dependent base unresolved; it offers what it names.
                    auto* using_decl =
                        llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(candidate.Declaration);
                    auto targets = using_decl ? resolver.lookup(using_decl)
                                              : types::TemplateResolver::lookup_result();
                    if(targets.empty()) {
                        add_declaration(candidate, 1.0F);
                        break;
                    }
                    for(auto* target: targets) {
                        auto named = candidate;
                        named.Declaration = target->getUnderlyingDecl();
                        add_declaration(named, 1.0F);
                    }
                    break;
                }
            }
        }

        // Where a member declaration starts, as Sema offers its override
        // declarations.
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(sema.CurContext);
        if(record && context.getKind() == clang::CodeCompletionContext::CCC_ClassStructUnion &&
           sema.getCurScope()->isClassScope()) {
            auto policy = completion_policy(sema);
            for(const auto* method: overridable(*record)) {
                clang::CodeCompletionBuilder builder(getAllocator(), getCodeCompletionTUInfo());
                auto& pattern =
                    *clang::CodeCompletionResult(method, clang::CCP_CodePattern)
                         .createCodeCompletionStringForOverride(sema.getPreprocessor(),
                                                                sema.getASTContext(),
                                                                builder,
                                                                /*IncludeBriefComments=*/false,
                                                                context,
                                                                policy);
                add({
                    .label = pattern.getAllTypedText(),
                    .kind = protocol::CompletionItemKind::Method,
                    .insert = pattern_text(pattern, /*whole=*/false) + ";",
                    .relevance = priority_weight(clang::CCP_CodePattern),
                });
            }
        }

        // The names of other namespaces the prefix matches, with the
        // qualifier they need, or with the `import` or the `#include` the
        // file needs to reach them.
        if(options.all_scopes && !prefix.spelling.empty() && !resolving && unqualified) {
            auto& ast = sema.getASTContext();
            // The namespaces around the completion point, outermost first: a
            // qualifier leaves them out.
            llvm::SmallVector<const clang::NamespaceDecl*, 4> around;
            for(const auto* scope = sema.CurContext; scope; scope = scope->getParent()) {
                auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
                if(space && !space->isAnonymousNamespace() && !space->isInline()) {
                    around.push_back(space->getCanonicalDecl());
                }
            }
            ranges::reverse(around);

            /// Whether a namespace or a class between the completion point
            /// and the namespace `shared` deep around it declares a scope
            /// named `name`: a qualifier starting with `name` names that
            /// one, and must start from the global scope instead.
            auto shadowed = [&](llvm::StringRef name, std::size_t shared) {
                const clang::Decl* home = shared == 0 ? nullptr : around[shared - 1];
                for(const auto* scope = sema.CurContext; !scope->isTranslationUnit();
                    scope = scope->getParent()) {
                    auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
                    if(space && space->getCanonicalDecl() == home) {
                        return false;
                    }
                    if(!space && !llvm::isa<clang::CXXRecordDecl>(scope)) {
                        continue;
                    }
                    for(auto* found: scope->lookup(&ast.Idents.get(name))) {
                        if(llvm::isa<clang::NamespaceDecl,
                                     clang::NamespaceAliasDecl,
                                     clang::TypeDecl>(found) &&
                           sema.isVisible(found)) {
                            return true;
                        }
                    }
                }
                return false;
            };

            /// The qualifier naming `decl` from the completion point; none
            /// for a member of a class, or for what needs none.
            auto qualifier_of =
                [&](const clang::NamedDecl* decl) -> std::optional<clang::NestedNameSpecifier> {
                llvm::SmallVector<const clang::NamespaceDecl*, 4> spaces;
                for(const auto* scope = decl->getDeclContext(); !scope->isTranslationUnit();
                    scope = scope->getParent()) {
                    if(auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
                        if(!space->isInline() && !space->isAnonymousNamespace()) {
                            spaces.push_back(space);
                        }
                    } else if(!scope->isTransparentContext()) {
                        return std::nullopt;
                    }
                }
                ranges::reverse(spaces);
                std::size_t shared = 0;
                while(shared < spaces.size() && shared < around.size() &&
                      spaces[shared]->getCanonicalDecl() == around[shared]) {
                    shared += 1;
                }
                if(shared == spaces.size()) {
                    return std::nullopt;
                }
                clang::NestedNameSpecifier qualifier = std::nullopt;
                if(shadowed(spaces[shared]->getName(), shared)) {
                    qualifier = clang::NestedNameSpecifier::getGlobal();
                    shared = 0;
                }
                for(auto* space: views::drop(spaces, shared)) {
                    qualifier = clang::NestedNameSpecifier(ast, space, qualifier);
                }
                return qualifier;
            };

            /// What a namespace scope offers: not a name a using-declaration
            /// brings in from another one, nor a specialization of a
            /// template it offers already.
            auto offerable = [](const clang::NamedDecl* decl) {
                if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
                    return !function->isFunctionTemplateSpecialization();
                }
                return llvm::isa<clang::FunctionTemplateDecl,
                                 clang::VarDecl,
                                 clang::VarTemplateDecl,
                                 clang::TagDecl,
                                 clang::TypedefNameDecl,
                                 clang::ClassTemplateDecl,
                                 clang::TypeAliasTemplateDecl,
                                 clang::ConceptDecl,
                                 clang::EnumConstantDecl,
                                 clang::NamespaceDecl,
                                 clang::NamespaceAliasDecl>(decl) &&
                       !llvm::isa<clang::ClassTemplateSpecializationDecl,
                                  clang::VarTemplateSpecializationDecl>(decl);
            };
            /// Where only a type may be written, what names one, or a scope.
            bool types_only = context.getKind() == clang::CodeCompletionContext::CCC_Type;
            auto names_type = llvm::IsaPred<clang::TypeDecl,
                                            clang::ClassTemplateDecl,
                                            clang::TypeAliasTemplateDecl,
                                            clang::ConceptDecl,
                                            clang::NamespaceDecl,
                                            clang::NamespaceAliasDecl>;

            /// The names this compile parsed in `scope` and in the namespaces
            /// in it, but those a reserved name keeps internal. A preamble's
            /// come from the index instead (ScopeCandidate): reading them all
            /// back from it would cost each completion more than the compile.
            auto scan = [&](this auto& self, clang::DeclContext* scope) -> void {
                for(auto* decl: scope->noload_decls()) {
                    if(decl->isFromASTFile()) {
                        continue;
                    }
                    auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
                    auto* identifier = named ? named->getIdentifier() : nullptr;
                    if(auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
                        if(space->isInline() || !space->getName().starts_with("_")) {
                            self(space);
                        }
                    } else if(auto* context = llvm::dyn_cast<clang::DeclContext>(decl);
                              context && context->isTransparentContext()) {
                        self(context);
                    }
                    if(!identifier || !match(identifier->getName()) || !offerable(named) ||
                       (types_only && !names_type(named)) ||
                       offered.contains(named->getCanonicalDecl()) || !sema.isVisible(named)) {
                        continue;
                    }
                    if(auto qualifier = qualifier_of(named)) {
                        clang::CodeCompletionResult result(named,
                                                           clang::CCP_Declaration,
                                                           *qualifier,
                                                           /*QualifierIsInformative=*/false);
                        add_declaration(result, 0.5F);
                    }
                }
            };
            scan(ast.getTranslationUnitDecl());

            auto import_at = action::import_insertion(content, sema.getLangOpts());
            llvm::StringRef own = module ? module->getPrimaryModuleInterfaceName() : "";
            // As qualified lookup finds them: through the inline and the
            // anonymous namespaces in the scope too.
            auto visible = [&](clang::DeclContext* scope, llvm::StringRef name) {
                clang::LookupResult found(sema,
                                          &ast.Idents.get(name),
                                          {},
                                          clang::Sema::LookupOrdinaryName);
                found.suppressDiagnostics();
                sema.LookupQualifiedName(found, scope);
                return llvm::SmallVector<clang::NamedDecl*, 2>(found.begin(), found.end());
            };
            auto& language = sema.getLangOpts();
            for(const auto& candidate: names.candidates) {
                // C names no namespace, an enumerator without its
                // enumeration, and a struct, union or enumeration only after
                // its keyword; a file compiled without named modules imports
                // nothing.
                using Kind = protocol::CompletionItemKind;
                llvm::ArrayRef<std::string> segments = candidate.scope;
                if(!language.CPlusPlus && candidate.kind == Kind::EnumMember) {
                    segments = segments.drop_back();
                }
                if((!language.CPlusPlus && (!segments.empty() || candidate.kind == Kind::Class ||
                                            candidate.kind == Kind::Enum)) ||
                   (!language.CPlusPlusModules && !candidate.module.empty()) ||
                   ranges::any_of(segments, [](llvm::StringRef segment) {
                       return segment.starts_with("_");
                   })) {
                    continue;
                }
                // Sema offers the macros defined here already, and no
                // include brings back one the file undefined; a macro is no
                // declaration to look up.
                if(candidate.macro &&
                   sema.getPreprocessor().getIdentifierInfo(candidate.name)->hadMacroDefinition()) {
                    continue;
                }
                clang::DeclContext* scope =
                    candidate.macro ? nullptr : ast.getTranslationUnitDecl();
                for(const auto& segment: segments) {
                    auto found = visible(scope, segment);
                    auto space =
                        ranges::find_if(found,
                                        llvm::IsaPred<clang::NamespaceDecl, clang::EnumDecl>);
                    scope = space == found.end() ? nullptr : llvm::cast<clang::DeclContext>(*space);
                    if(!scope) {
                        break;
                    }
                }
                if(scope) {
                    auto found = visible(scope, candidate.name);
                    for(auto* decl: found) {
                        decl = decl->getUnderlyingDecl();
                        if(offered.contains(decl->getCanonicalDecl()) ||
                           (types_only && !names_type(decl))) {
                            continue;
                        }
                        if(auto qualifier = qualifier_of(decl)) {
                            clang::CodeCompletionResult named(decl,
                                                              clang::CCP_Declaration,
                                                              *qualifier,
                                                              /*QualifierIsInformative=*/false);
                            add_declaration(named, 0.5F);
                        }
                    }
                    if(!found.empty()) {
                        continue;
                    }
                }

                // The file does not reach it: an import or an include will,
                // but none brings a whole namespace.
                if(candidate.kind == Kind::Module ||
                   (types_only && candidate.kind != Kind::Class && candidate.kind != Kind::Enum &&
                    candidate.kind != Kind::TypeParameter)) {
                    continue;
                }
                bool imported = !candidate.module.empty();
                if(imported ? !options.insert_imports : !options.insert_includes) {
                    continue;
                }
                std::string import;
                if(imported) {
                    auto [primary, partition] = llvm::StringRef(candidate.module).split(':');
                    if(primary != own) {
                        import = std::format("import {};\n", primary);
                    } else if(!partition.empty() && candidate.module != module->Name) {
                        import = std::format("import :{};\n", partition);
                    } else {
                        continue;
                    }
                }
                std::size_t shared = 0;
                while(shared < segments.size() && shared < around.size() &&
                      segments[shared] == around[shared]->getName()) {
                    shared += 1;
                }
                std::string qualifier;
                if(shared < segments.size() && shadowed(segments[shared], shared)) {
                    qualifier = "::";
                    shared = 0;
                }
                for(const auto& segment: views::drop(segments, shared)) {
                    qualifier += segment + "::";
                }
                Candidate item{
                    .label = qualifier + candidate.name,
                    .kind = candidate.kind,
                    .filter = candidate.name,
                    .deprecated = candidate.deprecated,
                    .relevance = 0.25F * priority_weight(clang::CCP_Declaration),
                };
                bool function = candidate.kind == protocol::CompletionItemKind::Function;
                if(options.bundle_overloads && function) {
                    item.bundle = item.label;
                }
                // Not knowing the parameters, a call takes its parentheses.
                if(argument_style != CompletionArguments::None && !arguments_follow) {
                    if(function && !expects_function(context.getPreferredType())) {
                        item.insert = client.snippets ? escape_snippet(item.label) + "($0)"
                                                      : item.label + "()";
                        item.snippet = client.snippets;
                        item.bundled_insert = item.insert;
                        item.bundled_snippet = item.snippet;
                    } else if(candidate.is_template && client.snippets && !function) {
                        item.insert = escape_snippet(item.label) + "<$0>";
                        item.snippet = true;
                    }
                }
                if(imported) {
                    item.edits.push_back({
                        .range = {import_at, import_at},
                        .text = std::move(import)
                    });
                } else {
                    item.include = candidate.symbol;
                }
                add(item);
            }
        }

        // SemaCodeComplete's `import name;` pattern skips implementation
        // units, which import past their module declaration just the same.
        if(context.getKind() == clang::CodeCompletionContext::CCC_TopLevel &&
           sema.CurContext->isTranslationUnit() && imports) {
            if(module && (module->Kind == clang::Module::ModuleImplementationUnit ||
                          module->Kind == clang::Module::ModulePartitionImplementation)) {
                clang::CodeCompletionBuilder builder(getAllocator(), getCodeCompletionTUInfo());
                builder.AddTypedTextChunk("import");
                builder.AddChunk(clang::CodeCompletionString::CK_HorizontalSpace);
                builder.AddPlaceholderChunk("name");
                builder.AddChunk(clang::CodeCompletionString::CK_SemiColon);
                builder.AddChunk(clang::CodeCompletionString::CK_VerticalSpace);
                add_pattern(*builder.TakeString(), clang::CCP_CodePattern);
            }
        }

        for(auto& entry: overloads) {
            if(entry.count > 1) {
                protocol::CompletionItemLabelDetails details;
                details.detail =
                    std::format("(…) +{} overload{}", entry.count - 1, entry.count > 2 ? "s" : "");
                entry.item.label_details = std::move(details);
                if(!entry.bundled_insert.empty()) {
                    std::visit([&](auto& edit) { edit.new_text = std::move(entry.bundled_insert); },
                               *entry.item.text_edit);
                    if(entry.bundled_snippet) {
                        entry.item.insert_text_format = protocol::InsertTextFormat::Snippet;
                    }
                }
            }
            collected.push_back({.item = std::move(entry.item),
                                 .dedup_priority = entry.dedup_priority,
                                 .documented = entry.documented});
        }

        // In bundle mode, deduplicate same-label candidates (see dedup_priority).
        if(options.bundle_overloads) {
            std::unordered_map<std::string, std::size_t> label_index;
            std::vector<CollectedItem> deduped;
            deduped.reserve(collected.size());

            for(auto& entry: collected) {
                // The variants of one statement share their keyword.
                if(entry.item.kind == protocol::CompletionItemKind::Snippet) {
                    deduped.push_back(std::move(entry));
                    continue;
                }
                auto [it, inserted] = label_index.try_emplace(entry.item.label, deduped.size());
                if(inserted) {
                    deduped.push_back(std::move(entry));
                } else if(entry.dedup_priority > deduped[it->second].dedup_priority) {
                    deduped[it->second] = std::move(entry);
                }
            }
            collected.swap(deduped);
        }

        output.clear();
        documented.clear();
        output.reserve(collected.size());
        for(auto& entry: collected) {
            output.push_back(std::move(entry.item));
            documented.push_back(entry.documented);
        }
    }

private:
    CompilationUnit& unit;
    FocusFile focus;
    std::uint32_t offset;
    PositionEncoding encoding;
    std::vector<protocol::CompletionItem>& output;

    /// What documents each item of `output`, in its order; read once the
    /// compile is done.
    std::vector<const clang::NamedDecl*>& documented;

    const CodeCompletionOptions& options;
    CompletionArguments argument_style;
    const CompletionClient& client;
    const CompileCompletion& names;
    /// Whether the candidates' names are still to load (load_names).
    bool names_pending;
    const InstantiatedTemplates& instantiated;
    /// Set once an item needs the header of its symbol included (Completion).
    bool& includes_pending;
    /// Whether Sema completes a member after `.` or `->`, where the
    /// candidates play no part.
    bool& member_access;
    /// Whether the qualifier's namespace brings in names of others
    /// (load_names).
    bool& brought_in;
    clang::CodeCompletionTUInfo info;
    bool resolving = false;
    bool resolved_candidates = false;

    std::optional<display::ReadableTypes> readable;
    display::Options display_options = {
        .suppress_unwritten_scope = true,
        .clean_uglified = true,
    };
};

}  // namespace

auto code_complete(CompilationParams params,
                   const CodeCompletionOptions& options,
                   const DocumentationOptions& documentation,
                   const CompletionClient& client,
                   PositionEncoding encoding,
                   const CompileCompletion& names,
                   const InstantiatedTemplates& instantiated) -> Completion {
    auto offset = params.completion.offset;

    std::vector<protocol::CompletionItem> items;
    std::vector<const clang::NamedDecl*> documented;
    bool includes = false;
    bool member_access = false;
    bool brought_in = false;
    // The index holds no module's names a module file brings in.
    bool from_index = names.from_index && params.input.pcms.empty() &&
                      (!names.preamble || *names.preamble == params.input.pch.second);
    params.completion.consumer = [&](CompilationUnit& unit) {
        return std::make_unique<Builder>(unit,
                                         offset,
                                         encoding,
                                         items,
                                         documented,
                                         options,
                                         client,
                                         names,
                                         from_index,
                                         instantiated,
                                         includes,
                                         member_access,
                                         brought_in);
    };
    auto unit = compile(std::move(params));

    assert(ranges::size(items) == ranges::size(documented));
    for(auto [item, decl]: views::zip(items, documented)) {
        if(!decl) {
            continue;
        }
        auto doc = feature::documentation(*unit, decl, documentation);
        if(doc.empty()) {
            continue;
        }
        auto shown = present(doc);
        item.documentation = protocol::MarkupContent{
            .kind =
                client.markdown ? protocol::MarkupKind::Markdown : protocol::MarkupKind::PlainText,
            .value = client.markdown ? shown.as_markdown() : shown.as_plain_text(),
        };
    }

    Completion completion{
        .list = completion_list(std::move(items), includes ? 0U : options.limit.value)};
    completion.list.is_incomplete =
        completion.list.is_incomplete ||
        ((names.more_candidates || (names.more_elsewhere && brought_in)) && !member_access);
    if(includes) {
        auto insertion = action::include_insertion(*unit);
        if(auto position = unit->focus_file().positions().position(insertion.offset, encoding)) {
            completion.include = IncludePlace{
                .position = *position,
                .opens_fragment = insertion.opens_fragment,
                .c = !unit->lang_options().CPlusPlus,
            };
        }
    }
    return completion;
}

auto instantiated_templates(CompilationUnit& unit) -> InstantiatedTemplates {
    InstantiatedTemplates result;
    auto& source_manager = unit.context().getSourceManager();
    auto record = [&](std::vector<InstantiatedTemplates::Entry>& into,
                      const clang::NamedDecl* decl,
                      auto describe) {
        auto [file, offset] =
            source_manager.getDecomposedLoc(source_manager.getFileLoc(decl->getLocation()));
        if(file != source_manager.getMainFileID()) {
            return;
        }
        InstantiatedTemplates::Entry entry{.offset = offset, .name = decl->getNameAsString()};
        Describer describer(entry.parts);
        if(describe(describer)) {
            into.push_back(std::move(entry));
        }
    };
    auto function = [&](const clang::FunctionDecl* pattern) {
        if(!pattern->getDescribedFunctionTemplate()) {
            return;
        }
        auto* instantiation = llvm::dyn_cast_or_null<clang::FunctionDecl>(
            decls::only_instantiation(const_cast<clang::FunctionDecl*>(pattern)));
        if(!instantiation) {
            return;
        }
        record(result.functions, pattern, [&](Describer& describer) {
            return ranges::all_of(
                instantiation->getTemplateSpecializationArgs()->asArray(),
                [&](const clang::TemplateArgument& each) { return describer.argument(each); });
        });
    };
    types::TemplateResolver resolver(unit.context());
    for(const auto& entry: unit.get<Semantics>().node_entries()) {
        // The preprocessor segment follows the AST segment.
        if(!entry.node.is_ast()) {
            break;
        }
        if(entry.flags.in_instantiation) {
            continue;
        }
        if(entry.node.kind() == SemanticNode::Kind::Stmt) {
            if(const auto* lambda =
                   llvm::dyn_cast<clang::LambdaExpr>(entry.node.get<clang::Stmt>())) {
                function(lambda->getCallOperator());
            }
            continue;
        }
        if(entry.node.kind() != SemanticNode::Kind::Decl) {
            continue;
        }
        const auto* decl = entry.node.get<clang::Decl>();
        if(const auto* pattern = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            function(pattern);
        } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
                  var && var->isCXXForRangeDecl() && var->getType()->isDependentType() &&
                  var->getType()->getContainedAutoType()) {
            auto type = resolver.deduce(var);
            auto* tag = type.isNull() ? nullptr : resolver.resolve_tag(type.getNonReferenceType());
            if(tag) {
                record(result.loops, var, [&](Describer& describer) { return describer.tag(tag); });
            }
        }
    }
    return result;
}

}  // namespace clice::feature
