module clice;

import :feature.code_completion.cursor;
import :feature.code_completion.reply;
import :feature.code_completion;
import :semantic.display;
import :semantic.identity;
import :semantic.template_resolver;
import :support.json;
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

    if(llvm::isa<clang::CXXMethodDecl,
                 clang::CXXConversionDecl,
                 clang::CXXDestructorDecl,
                 clang::ObjCMethodDecl>(decl)) {
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
                 clang::ClassTemplateSpecializationDecl,
                 clang::ObjCInterfaceDecl,
                 clang::ObjCCategoryDecl>(decl)) {
        return protocol::CompletionItemKind::Class;
    }

    if(llvm::isa<clang::ObjCProtocolDecl>(decl)) {
        return protocol::CompletionItemKind::Interface;
    }

    if(llvm::isa<clang::ObjCPropertyDecl>(decl)) {
        return protocol::CompletionItemKind::Property;
    }

    if(llvm::isa<clang::TypedefNameDecl,
                 clang::ObjCCompatibleAliasDecl,
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
    // A class template's name is an ordinary one too.
    const clang::Decl* own = nullptr;
    if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(tag)) {
        if(auto* pattern = record->getDescribedClassTemplate()) {
            own = pattern->getCanonicalDecl();
        }
    }
    return ranges::any_of(scope->lookup(tag->getDeclName()), [&](const clang::NamedDecl* found) {
        return found->getCanonicalDecl() != own &&
               found->isInIdentifierNamespace(clang::Decl::IDNS_Ordinary);
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

/// Whether a function declared in a class derived from both overrides
/// either: the same name, parameter types and qualifiers.
bool overrides_alike(const clang::CXXMethodDecl& lhs, const clang::CXXMethodDecl& rhs) {
    if(lhs.getDeclName() != rhs.getDeclName() || lhs.getNumParams() != rhs.getNumParams() ||
       lhs.isVariadic() != rhs.isVariadic() ||
       lhs.getMethodQualifiers() != rhs.getMethodQualifiers() ||
       lhs.getRefQualifier() != rhs.getRefQualifier()) {
        return false;
    }
    auto& ast = lhs.getASTContext();
    return ranges::all_of(views::zip(lhs.parameters(), rhs.parameters()), [&](auto pair) {
        auto [left, right] = pair;
        return ast.hasSameUnqualifiedType(left->getType(), right->getType());
    });
}

/// The virtual functions of `record`'s bases, direct or not, an override
/// declaration in its body may name: none that it, or a class between it and
/// the base, overrides, and none final or deleted, nor one a final function
/// of another base would be overridden with. Sema offers only its direct
/// bases'.
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

    llvm::SmallVector<const clang::CXXMethodDecl*> finals;
    for(const auto* base: bases) {
        for(const auto* method: base->methods()) {
            if(method->isVirtual() && method->hasAttr<clang::FinalAttr>()) {
                finals.push_back(method);
            }
        }
    }
    llvm::SmallVector<const clang::CXXMethodDecl*> methods;
    for(const auto* base: bases) {
        for(const auto* method: base->methods()) {
            if(method->isVirtual() && method->getIdentifier() && !method->isDeleted() &&
               !method->hasAttr<clang::FinalAttr>() &&
               !overridden.contains(method->getCanonicalDecl()) &&
               ranges::none_of(finals, [&](const clang::CXXMethodDecl* final) {
                   return overrides_alike(*final, *method);
               })) {
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

/// Whether `text` starts with a brace group the next initializer's `,`,
/// a pack's `...` or the constructor's body follows, rather than with the
/// body itself.
bool braced_initializer(llvm::StringRef text, const clang::LangOptions& options) {
    Lexer lexer(text, options);
    if(lexer.advance().kind != clang::tok::l_brace) {
        return false;
    }
    for(std::uint32_t depth = 1; depth != 0;) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            return false;
        }
        if(token.kind == clang::tok::l_brace) {
            depth += 1;
        } else if(token.kind == clang::tok::r_brace) {
            depth -= 1;
        }
    }
    auto next = lexer.advance().kind;
    return next == clang::tok::comma || next == clang::tok::l_brace || next == clang::tok::ellipsis;
}

/// What of `name`, a base's or a member's, the name typed stands for when
/// `rest`, the text after it, writes the rest of a name: the head of `name`
/// whose tail `rest` starts with, `ns` of `ns::Seed` before `::Seed(1)`;
/// otherwise `name` without the template arguments it ends in, which `rest`
/// spells another way.
llvm::StringRef written_head(llvm::StringRef name, llvm::StringRef rest) {
    for(std::size_t i = 1; i < name.size(); i += 1) {
        auto tail = name.drop_front(i);
        if((tail.starts_with("<") || tail.starts_with("::")) && rest.starts_with(tail)) {
            return name.take_front(i);
        }
    }
    if(!name.ends_with(">")) {
        return name;
    }
    std::uint32_t depth = 0;
    for(auto i = name.size(); i != 0; i -= 1) {
        if(name[i - 1] == '>') {
            depth += 1;
        } else if(name[i - 1] == '<' && --depth == 0) {
            return name.take_front(i - 1);
        }
    }
    return name;
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
/// "ClassName<${1:T}>" for class templates, "moveToX:${1:(int)} y:${2:(int)}"
/// for a message, or a whole statement for a keyword pattern; its tab stops
/// are empty unless `named`. Empty when there is nothing to fill in.
auto build_snippet(const clang::CodeCompletionString& ccs, bool named = true) -> std::string {
    std::string snippet;
    unsigned placeholder_index = 0;

    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        switch(chunk.Kind) {
            case CK::CK_Placeholder:
                placeholder_index += 1;
                if(named) {
                    snippet += std::format("${{{}:{}}}",
                                           placeholder_index,
                                           escape_snippet(chunk.Text, /*placeholder=*/true));
                } else {
                    snippet += std::format("${}", placeholder_index);
                }
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
/// tells apart the two `for` statements. A message's selector is typed
/// piece by piece: its tail is the pieces after the first, each with its
/// argument.
auto pattern_tail(const clang::CodeCompletionString& ccs) -> std::string {
    std::string tail;
    bool typed = false;
    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        if(typed && chunk.Kind != CK::CK_Optional && chunk.Kind != CK::CK_Informative) {
            tail += chunk.Kind == CK::CK_VerticalSpace ? " " : chunk.Text;
        }
        typed = typed || chunk.Kind == CK::CK_TypedText;
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

/// What a message still writes of `method`'s selector, the pieces before
/// `start` written already: `moveToX:y:` from the start, `y:` after
/// `moveToX:1`. Empty when every piece is written.
auto selector_rest(const clang::ObjCMethodDecl& method, unsigned start) -> std::string {
    auto selector = method.getSelector();
    if(selector.isUnarySelector()) {
        return selector.getNameForSlot(0).str();
    }
    std::string rest;
    for(auto piece = start; piece < selector.getNumArgs(); piece += 1) {
        rest += selector.getNameForSlot(piece);
        rest += ':';
    }
    return rest;
}

}  // namespace

protocol::CompletionItemLabelDetails bundle_details(std::size_t count) {
    protocol::CompletionItemLabelDetails details;
    details.detail = std::format("(…) +{} overload{}", count - 1, count > 2 ? "s" : "");
    return details;
}

auto Reply::open(Completing& completing, clang::CodeCompletionContext& context)
    -> std::optional<Reply> {
    auto prefix = Prefix::from(completing.text, completing.offset);
    auto map = completing.unit.focus_file().positions();
    auto typed = map.range(prefix.typed, completing.encoding);
    auto whole = map.range(prefix.whole, completing.encoding);
    if(!typed || !whole) {
        return std::nullopt;
    }
    Edit edit = protocol::TextEdit{.range = *whole};
    if(completing.client.insert_replace && prefix.whole != prefix.typed) {
        edit = protocol::InsertReplaceEdit{.insert = *typed, .replace = *whole};
    }
    return Reply(completing, context, prefix, std::move(edit));
}

Reply::Reply(Completing& completing,
             clang::CodeCompletionContext& context,
             Prefix prefix,
             Edit edit) :
    completing(completing), sema(completing.unit.sema()), context(context), prefix(prefix),
    matcher(prefix.spelling), edit(std::move(edit)) {
    auto text = completing.text;
    auto after = text.drop_front(prefix.whole.end).ltrim();
    follows = after.starts_with("(")   ? Follows::Parenthesis
              : after.starts_with("<") ? Follows::Angle
              : after.starts_with(":") ? Follows::Colon
              : constructor_initializers() && braced_initializer(after, sema.getLangOpts())
                  ? Follows::BracedInitializer
                  : Follows::Nothing;

    auto* module = sema.getCurrentModule();
    bool named_module = module && module->isNamedModule();
    auto focus = completing.unit.focus_file();
    auto completion_loc = focus.location(completing.offset);
    auto declared = ranges::any_of(
        sema.getASTContext().getTranslationUnitDecl()->noload_decls(),
        [&](const clang::Decl* decl) {
            return !decl->isImplicit() && !llvm::isa<clang::ImportDecl>(decl) &&
                   (named_module ? decl->isInNamedModule()
                                 : focus.written_offset(decl->getLocation()).has_value()) &&
                   completing.unit.before(decl->getLocation(), completion_loc);
        });
    imports = named_module ? !declared
                           : !in_global_fragment(text, prefix.typed.begin, sema.getLangOpts());
    exports = named_module ? module->isModuleInterfaceUnit() : !declared;

    if(member_access() && !context.getBaseExpr()) {
        auto& [order, designated] = designators;
        if(auto* record = llvm::dyn_cast_or_null<clang::RecordDecl>(
               clang::HeuristicResolver(sema.getASTContext())
                   .resolveTypeToTagDecl(context.getBaseType()))) {
            order = designator_order(record);
        }
        std::uint32_t passed = 0;
        for(auto designator: written_designators(text, prefix.typed.begin, sema.getLangOpts())) {
            if(designator.whole) {
                designated.insert(designator.field);
            }
            if(auto it = order.find(designator.field);
               sema.getLangOpts().CPlusPlus && it != order.end()) {
                passed = std::max(passed, it->second);
            }
        }
        for(auto& [name, place]: order) {
            if(place < passed) {
                designated.insert(name);
            }
        }
    }
}

bool Reply::member_access() const {
    return context.getKind() == clang::CodeCompletionContext::CCC_DotMemberAccess ||
           context.getKind() == clang::CodeCompletionContext::CCC_ArrowMemberAccess;
}

bool Reply::unqualified() const {
    using Kind = clang::CodeCompletionContext::Kind;
    switch(context.getKind()) {
        case Kind::CCC_TopLevel:
        case Kind::CCC_ClassStructUnion:
        case Kind::CCC_Statement:
        case Kind::CCC_Expression:
        case Kind::CCC_ParenthesizedExpression:
        case Kind::CCC_Type:
        case Kind::CCC_TopLevelOrExpression: return true;
        // Unqualified, a symbol is a constructor's initializer: a base or a
        // member, never another namespace's name.
        case Kind::CCC_Symbol: return false;
        case Kind::CCC_SymbolOrNewName: {
            auto spec = context.getCXXScopeSpecifier();
            return !spec || (*spec)->isEmpty();
        }
        default: return false;
    }
}

bool Reply::constructor_initializers() const {
    return context.getKind() == clang::CodeCompletionContext::CCC_Symbol &&
           !context.getCXXScopeSpecifier();
}

bool Reply::expects_function() const {
    auto preferred = context.getPreferredType();
    if(preferred.isNull()) {
        return false;
    }
    preferred = preferred.getNonReferenceType();
    if(preferred->isMemberPointerType() || preferred->isFunctionType()) {
        return true;
    }
    return preferred->isPointerType() && preferred->getPointeeType()->isFunctionType();
}

bool Reply::arguments_follow(bool templated) const {
    return follows == Follows::Parenthesis || (follows == Follows::Angle && templated);
}

bool Reply::allowed(llvm::StringRef keyword) const {
    return (keyword != "import" || imports) && (keyword != "export" || exports);
}

bool Reply::offers_requirements() const {
    if(member_access()) {
        return llvm::isa<clang::TemplateTypeParmType>(context.getBaseType());
    }
    if(auto spec = context.getCXXScopeSpecifier()) {
        auto scope = (*spec)->getScopeRep();
        return scope.getKind() == clang::NestedNameSpecifier::Kind::Type &&
               llvm::isa<clang::TemplateTypeParmType>(scope.getAsType());
    }
    return false;
}

std::optional<float> Reply::match(llvm::StringRef filter) {
    // Filter out _/__ prefixed internal symbols unless user typed _.
    if(filter.empty() || (filter.starts_with("_") && !prefix.spelling.starts_with("_"))) {
        return std::nullopt;
    }
    return matcher.match(filter);
}

protocol::CompletionItem Reply::item_of(const Candidate& candidate, float score) {
    protocol::CompletionItem item{
        .label = candidate.label,
    };
    item.kind = candidate.kind;
    auto text = candidate.insert.empty() ? candidate.label : candidate.insert;
    // A statement spanning lines follows the indentation it lands at.
    if(text.find('\n') != std::string::npos) {
        item.insert_text_mode = protocol::InsertTextMode::AdjustIndentation;
    }
    item.text_edit = edit;
    std::visit([&](auto& replaced) { replaced.new_text = std::move(text); }, *item.text_edit);
    if(candidate.snippet) {
        item.insert_text_format = protocol::InsertTextFormat::Snippet;
    }
    if(!candidate.filter.empty()) {
        item.filter_text = candidate.filter;
    }
    item.sort_text = sort_text(score, candidate.order);
    if(!candidate.edits.empty()) {
        auto map = completing.unit.focus_file().positions();
        std::vector<protocol::TextEdit> edits;
        for(const auto& replacement: candidate.edits) {
            if(auto range = map.range(replacement.range, completing.encoding)) {
                edits.push_back({.range = *range, .new_text = replacement.text});
            }
        }
        item.additional_text_edits = std::move(edits);
    }
    if(candidate.include != 0) {
        item.data = protocol::LSPAny(std::format("{}", candidate.include));
        completing.includes_pending = true;
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
}

void Reply::add_item(const Candidate& candidate) {
    auto matched = match(candidate.filter.empty() ? candidate.label : candidate.filter);
    if(!matched) {
        return;
    }
    auto score = *matched * candidate.relevance;
    if(candidate.deprecated) {
        score *= 0.5F;
    }

    // A name the file does not reach yet yields to one it does.
    int priority = candidate.include != 0 ? -1 : dedup_priority(candidate.kind, candidate.macro);

    if(!candidate.bundle.empty()) {
        auto [it, inserted] = bundles.try_emplace(candidate.bundle);
        auto& overloads = it->second;
        if(inserted) {
            overloads = Bundle{
                .item = item_of(candidate, score),
                .score = score,
                .count = 1,
                .priority = priority,
                .bundled_insert = candidate.bundled_insert,
                .bundled_snippet = candidate.bundled_snippet,
                .documented = candidate.documented,
            };
        } else {
            overloads.count += 1;
            if(score > overloads.score) {
                overloads.score = score;
                overloads.item.sort_text = sort_text(score, std::nullopt);
            }
        }
        return;
    }

    items.push_back({
        .item = item_of(candidate, score),
        .priority = priority,
        .documented = candidate.documented,
    });
}

void Reply::add_candidates(llvm::MutableArrayRef<clang::CodeCompletionResult> candidates) {
    types::TemplateResolver resolver(sema.getASTContext());
    auto* naming = unchecked_naming_class(sema, context);
    for(auto [index, candidate]: views::enumerate(candidates)) {
        switch(candidate.Kind) {
            case clang::CodeCompletionResult::RK_Keyword:
                if(allowed(candidate.Keyword)) {
                    add_item({.label = candidate.Keyword,
                              .kind = protocol::CompletionItemKind::Keyword,
                              .relevance = priority_weight(candidate.Priority)});
                }
                break;

            case clang::CodeCompletionResult::RK_Pattern:
                if(constructor_initializers()) {
                    add_initializer(candidate, index);
                } else {
                    add_pattern(candidate);
                }
                break;

            case clang::CodeCompletionResult::RK_Macro: add_macro(candidate); break;

            case clang::CodeCompletionResult::RK_Declaration: {
                if(left_out(candidate, naming)) {
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
}

void Reply::add_pattern(const clang::CodeCompletionResult& candidate) {
    auto& pattern = *candidate.Pattern;
    // A pattern carrying a C++ method is an override declaration in a class
    // body, which the overrides offer for every base (add_overrides), or in a
    // method body a call of the overridden method — a duplicate of the
    // method's own candidate that would change which function is called.
    if(candidate.Declaration) {
        if(const auto* method = llvm::dyn_cast<clang::ObjCMethodDecl>(candidate.Declaration)) {
            add_method_pattern(pattern, *method, candidate.Priority);
        }
        return;
    }
    // After `@selector(` Sema offers every selector it knows, from the piece
    // the cursor is at.
    if(context.getKind() == clang::CodeCompletionContext::CCC_SelectorName) {
        auto typed = pattern.getAllTypedText();
        add_item({
            .label = typed,
            .kind = protocol::CompletionItemKind::Method,
            .insert = follows == Follows::Colon ? llvm::StringRef(typed).split(':').first.str()
                                                : std::string(),
            .relevance = priority_weight(candidate.Priority),
        });
        return;
    }
    if(offers_requirements()) {
        add_requirement(pattern, candidate.Priority);
        return;
    }
    // After `#ifdef` a macro comes as a pattern of its name, an include guard
    // among them, which Sema's other lists of macros leave out.
    if(context.getKind() == clang::CodeCompletionContext::CCC_MacroNameUse) {
        auto& pp = sema.getPreprocessor();
        auto* info = pp.getMacroInfo(pp.getIdentifierInfo(pattern.getTypedText()));
        if(info && info->isUsedForHeaderGuard()) {
            return;
        }
    }
    if(allowed(pattern.getTypedText())) {
        add_keyword_pattern(pattern, candidate.Priority);
    }
}

void Reply::add_initializer(const clang::CodeCompletionResult& candidate, std::uint32_t order) {
    const auto* pattern = candidate.Pattern;
    // LIMITATION: a virtual base is initialized before the other bases,
    // where Sema does not list it: with one, the order shown and the
    // initializer ranked next may be off.
    Candidate item{
        .label = pattern->getTypedText(),
        .kind = protocol::CompletionItemKind::Class,
        .documented = candidate.Declaration,
        .relevance = priority_weight(candidate.Priority),
        .order = order,
    };
    // A member's candidate carries its class, or the member itself when its
    // type is no class.
    auto fields = llvm::cast<clang::CXXConstructorDecl>(sema.CurContext)->getParent()->fields();
    if(auto field = ranges::find_if(fields,
                                    [&](const clang::FieldDecl* field) {
                                        const clang::NamedDecl* carried =
                                            field->getType()->getAsCXXRecordDecl();
                                        return field->getName() == item.label &&
                                               candidate.Declaration == (carried ? carried : field);
                                    });
       field != fields.end()) {
        item.kind = protocol::CompletionItemKind::Field;
        item.documented = *field;
        // For a member whose class has declared no constructor yet, as an
        // aggregate's implicit ones are declared when first needed, Sema
        // names the placeholder after the member rather than its type.
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(candidate.Declaration)) {
            auto& ast = sema.getASTContext();
            auto name = ast.DeclarationNames.getCXXConstructorName(ast.getCanonicalTagType(record));
            if(record->lookup(name).empty()) {
                using CK = clang::CodeCompletionString::ChunkKind;
                clang::CodeCompletionBuilder builder(completing.info.getAllocator(),
                                                     completing.info);
                auto type = (*field)->getType().getAsString(completion_policy(sema));
                builder.AddTypedTextChunk(builder.getAllocator().CopyString(item.label));
                builder.AddChunk(CK::CK_LeftParen);
                builder.AddPlaceholderChunk(builder.getAllocator().CopyString(type));
                builder.AddChunk(CK::CK_RightParen);
                pattern = builder.TakeString();
            }
        }
    }
    item.signature = extract_signature(*pattern, /*qualifiers=*/false);
    if(completing.options.bundle_overloads) {
        item.bundle = item.label;
    }
    // The name typed begins a base's name written already.
    if(follows == Follows::Colon || follows == Follows::Angle) {
        item.insert =
            written_head(item.label, completing.text.drop_front(prefix.whole.end).ltrim()).str();
    } else if(follows != Follows::BracedInitializer) {
        insert_call(item, *pattern, /*called_here=*/true);
    }
    add_item(item);
}

void Reply::add_keyword_pattern(const clang::CodeCompletionString& pattern, unsigned priority) {
    auto label = pattern.getAllTypedText();
    auto head = pattern_text(pattern, /*whole=*/false);
    if(completing.options.statement_snippets && completing.client.snippets) {
        auto snippet = build_snippet(pattern);
        bool has_snippet = !snippet.empty();
        add_item({
            .label = label,
            .kind = protocol::CompletionItemKind::Snippet,
            .insert = has_snippet ? std::move(snippet) : pattern_text(pattern, /*whole=*/true),
            .snippet = has_snippet,
            .signature = pattern_tail(pattern),
            .relevance = priority_weight(priority),
        });
    } else if(plain_patterns.insert(head).second) {
        // Without its placeholders a pattern is its keyword; the variants of
        // one statement collapse.
        add_item({
            .label = label,
            .kind = protocol::CompletionItemKind::Keyword,
            .insert = head,
            .relevance = priority_weight(priority),
        });
    }
}

void Reply::add_method_pattern(const clang::CodeCompletionString& pattern,
                               const clang::ObjCMethodDecl& method,
                               unsigned priority) {
    Candidate item{
        .kind = protocol::CompletionItemKind::Method,
        .return_type = extract_return_type(pattern),
        .documented = &method,
        .relevance = priority_weight(priority),
    };
    for(const auto& chunk: pattern) {
        using CK = clang::CodeCompletionString::ChunkKind;
        if(chunk.Kind == CK::CK_LeftBrace) {
            break;
        }
        if(chunk.Kind == CK::CK_Informative || chunk.Kind == CK::CK_ResultType) {
            continue;
        }
        item.insert += chunk.Text;
        if(!item.label.empty()) {
            item.signature += chunk.Text;
        } else if(chunk.Kind == CK::CK_TypedText) {
            item.label = chunk.Text;
        }
    }
    item.insert = follows == Follows::Colon ? llvm::StringRef(item.label).rtrim(':').str()
                                            : llvm::StringRef(item.insert).rtrim().str();
    item.signature = llvm::StringRef(item.signature).rtrim().str();
    if(auto typed = pattern.getAllTypedText(); typed != item.label) {
        item.filter = std::move(typed);
    }
    add_item(item);
}

void Reply::add_requirement(const clang::CodeCompletionString& pattern, unsigned priority) {
    Candidate item{
        .label = pattern.getAllTypedText(),
        .signature = extract_signature(pattern, /*qualifiers=*/false),
        .return_type = extract_return_type(pattern),
    };
    if(item.signature.empty()) {
        // After `T::` a name without a call is mostly a type the
        // constraints name, `typename T::value_type`.
        item.kind = member_access() ? protocol::CompletionItemKind::Field
                                    : protocol::CompletionItemKind::TypeParameter;
    } else {
        item.kind = protocol::CompletionItemKind::Method;
        insert_call(item, pattern, /*called_here=*/true);
    }
    item.relevance = priority_weight(priority);
    add_item(item);
}

void Reply::add_macro(clang::CodeCompletionResult& candidate) {
    if(!match(candidate.Macro->getName())) {
        return;
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
    if(unqualified() && info && completing.unit.in_library(info->getDefinitionLoc())) {
        item.relevance *= 0.5F;
    }
    if(auto* ccs = candidate.CreateCodeCompletionString(sema,
                                                        context,
                                                        completing.info.getAllocator(),
                                                        completing.info,
                                                        /*IncludeBriefComments=*/false)) {
        item.signature = extract_signature(*ccs, /*qualifiers=*/false);
        if(function_like) {
            insert_call(item, *ccs, /*called_here=*/true);
        }
    }
    add_item(item);
}

bool Reply::left_out(const clang::CodeCompletionResult& candidate,
                     const clang::CXXRecordDecl* naming) const {
    auto* declaration = candidate.Declaration;
    // A hidden declaration is not what its name means here.
    if(!declaration || candidate.Hidden || candidate.Availability == CXAvailability_NotAccessible) {
        return true;
    }
    // The members `.` reaches on an object with an `operator->`
    // are not what its `->` asks for.
    if(!candidate.FixIts.empty() &&
       context.getKind() == clang::CodeCompletionContext::CCC_ArrowMemberAccess) {
        return true;
    }
    // Clang gives a deduction guide no name to type.
    if(declaration->getDeclName().getNameKind() == clang::DeclarationName::CXXDeductionGuideName) {
        return true;
    }
    if(member_access() && rarely_accessed(declaration)) {
        return true;
    }
    if(auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration);
       field && designators.designated.contains(field->getName())) {
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
}

void Reply::add_declaration(clang::CodeCompletionResult& candidate, float relevance) {
    // Sema offers a class's constructors with its name.
    const auto* tag = llvm::dyn_cast<clang::TagDecl>(candidate.Declaration);
    if(auto* constructor = llvm::dyn_cast_or_null<clang::CXXConstructorDecl>(
           candidate.Declaration->getAsFunction())) {
        tag = constructor->getParent();
    }
    // After `struct`, `union` or `enum`, where Sema offers only tags.
    bool tags_only = ranges::contains(std::array{clang::CodeCompletionContext::CCC_ClassOrStructTag,
                                                 clang::CodeCompletionContext::CCC_UnionTag,
                                                 clang::CodeCompletionContext::CCC_EnumTag},
                                      context.getKind());
    if(tag && !tags_only && hidden_tag(tag)) {
        return;
    }
    candidate.Declaration = shown_declaration(candidate.Declaration);
    auto* declaration = candidate.Declaration;
    offered.insert(declaration->getCanonicalDecl());
    const auto* method = llvm::dyn_cast<clang::ObjCMethodDecl>(declaration);
    if(method && candidate.DeclaringEntity &&
       !declared_selectors.insert(method->getSelector().getAsString()).second) {
        return;
    }
    auto name = method ? selector_rest(*method, candidate.StartParameter)
                       : display::name_of(declaration,
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
    // A message's item is named by the selector's piece the cursor
    // is at, the pieces after it are its signature.
    if(method && ranges::count(name, ':') > 1) {
        item.label = name.substr(0, name.find(':') + 1);
        item.filter = name;
    }

    // Clang offers a namespace last, among the qualifiers; but one is
    // typed to name what it declares, like any other declaration.
    bool space = llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(declaration);
    auto priority =
        space ? std::min(candidate.Priority, static_cast<unsigned>(clang::CCP_Declaration))
              : candidate.Priority;
    // Clang weighs every name by the type expected but a member's,
    // and knows no type a nested run stands in for.
    if((member_access() || completing.expected_type != 0) && expected(declaration)) {
        priority /= clang::CCF_ExactTypeMatch;
    }
    item.relevance = relevance * priority_weight(priority);
    if(unqualified() && completing.unit.in_library(declaration->getLocation())) {
        item.relevance *= 0.5F;
    }
    if(member_access() && static_member(declaration)) {
        item.relevance *= 0.5F;
    }
    for(const auto& fix: candidate.FixIts) {
        if(auto removed = completing.unit.written_range(fix.RemoveRange);
           removed && completing.unit.focus_file().contains(removed->file)) {
            item.edits.push_back({.range = removed->range, .text = fix.CodeToInsert});
        }
    }
    if(!item.edits.empty()) {
        item.relevance *= 0.5F;
    }

    if(auto* field = llvm::dyn_cast<clang::FieldDecl>(declaration)) {
        if(auto it = designators.order.find(field->getName()); it != designators.order.end()) {
            item.order = it->second;
        }
    }

    bool is_callable = kind == protocol::CompletionItemKind::Function ||
                       kind == protocol::CompletionItemKind::Method ||
                       kind == protocol::CompletionItemKind::Constructor;
    if(completing.options.bundle_overloads && is_callable && !method) {
        item.bundle = item.label;
    }

    if(std::holds_alternative<NoLimit>(completing.options.limit)) {
        render(candidate, std::move(item));
        return;
    }
    auto score = *match(item.filter.empty() ? item.label : item.filter) * item.relevance;
    if(item.deprecated) {
        score *= 0.5F;
    }
    pending.push_back({candidate, std::move(item), score});
}

void Reply::render(clang::CodeCompletionResult& candidate, Candidate item) {
    auto& options = completing.options;
    auto& client = completing.client;
    auto& allocator = completing.info.getAllocator();
    auto* declaration = candidate.Declaration;
    auto kind = item.kind;
    bool is_callable = kind == protocol::CompletionItemKind::Function ||
                       kind == protocol::CompletionItemKind::Method ||
                       kind == protocol::CompletionItemKind::Constructor;
    clang::CodeCompletionBuilder builder(allocator, completing.info);
    const auto* pattern = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration);
    auto& ccs = pattern && !candidate.StartsNestedNameSpecifier
                    ? class_template_string(builder, candidate, *pattern, sema)
                    : *candidate.CreateCodeCompletionString(sema,
                                                            context,
                                                            allocator,
                                                            completing.info,
                                                            /*IncludeBriefComments=*/false);
    item.return_type = extract_return_type(ccs);
    if(llvm::isa<clang::ObjCMethodDecl>(declaration)) {
        item.signature = pattern_tail(ccs);
        if(candidate.DeclaringEntity) {
            // A method's declaration after its return type.
            item.insert = pattern_text(ccs, /*whole=*/true);
        } else if(follows == Follows::Colon) {
            item.insert = llvm::StringRef(item.label).rtrim(':').str();
        } else if(client.snippets && options.arguments != CompletionArguments::None) {
            auto snippet =
                build_snippet(ccs, options.arguments == CompletionArguments::Placeholders);
            if(!snippet.empty()) {
                item.insert = std::move(snippet);
                item.snippet = true;
            }
        }
        add_item(item);
        return;
    }
    // Clang gives a variable holding a lambda the signature of its
    // call operator, whose `const` is no part of calling it.
    bool method = llvm::isa_and_present<clang::CXXMethodDecl>(declaration->getAsFunction());
    std::size_t parameters;
    item.signature = extract_signature(ccs, /*qualifiers=*/method, &parameters);
    // Where Sema gives a parameter list and a type, they are printed
    // as every feature prints them.
    auto& display_options = completing.display_options;
    if(!completing.readable) {
        completing.readable.emplace(sema.getASTContext(), display_options);
        display_options.readable = &*completing.readable;
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
        } else if(const auto* enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(declaration)) {
            // C types an enumerator `int`, as C++ does inside its
            // enum: its enum is what it reads as.
            type = sema.getASTContext().getCanonicalTagType(
                llvm::cast<clang::EnumDecl>(enumerator->getDeclContext()));
        } else if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(declaration)) {
            type = value->getType();
        }
        if(!type.isNull()) {
            item.return_type = display::type(sema.getASTContext(), type, display_options).text;
        }
    }
    // LIMITATION: a function passed where a parameter takes a
    // `std::function` still gets its parentheses, as in clangd; a
    // variable holding a callable that returns one object pointer is
    // called where another object pointer is expected.
    auto preferred = context.getPreferredType();
    if(is_callable) {
        insert_call(item,
                    ccs,
                    kind != protocol::CompletionItemKind::Constructor &&
                        candidate.FunctionCanBeCall && !candidate.DeclaringEntity &&
                        !context.isUsingDeclaration() && !expects_function());
    } else if(item.signature.starts_with("(")) {
        // A variable holding a lambda is mostly passed on: it is
        // called where its call gives what the context expects.
        auto result = call_result(declaration);
        insert_call(item,
                    ccs,
                    preferred.isNull() || (!result.isNull() && !preferred->isDependentType() &&
                                           initializes(sema.getASTContext(), result, preferred)));
    } else if(options.arguments != CompletionArguments::None && client.snippets &&
              !arguments_follow(true) && !context.isUsingDeclaration() &&
              llvm::isa<clang::ClassTemplateDecl,
                        clang::TypeAliasTemplateDecl,
                        clang::VarTemplateDecl>(declaration)) {
        // LIMITATION: a class template given as a template template
        // argument (`Use<Vec|>`) gets its arguments too
        // (`Vec<${1:class T}>`), which the argument must not have.
        auto snippet = build_snippet(ccs);
        // Every parameter defaulted: nothing to fill in, but the
        // name alone does not name a type.
        if(snippet.empty()) {
            item.insert = item.label + "<>";
        } else if(options.arguments == CompletionArguments::Placeholders) {
            item.insert = std::move(snippet);
            item.snippet = true;
        } else {
            item.insert = escape_snippet(item.label) + "<$0>";
            item.snippet = true;
        }
    }

    add_item(item);
}

void Reply::insert_call(Candidate& item, const clang::CodeCompletionString& ccs, bool called_here) {
    auto& options = completing.options;
    if(arguments_follow(llvm::isa_and_present<clang::TemplateDecl>(item.documented)) ||
       !called_here || options.arguments == CompletionArguments::None) {
        return;
    }
    auto complete = item.label + "()";
    if(!completing.client.snippets) {
        item.insert = complete;
        item.bundled_insert = std::move(complete);
        return;
    }
    item.bundled_insert = escape_snippet(item.label) + "($0)";
    item.bundled_snippet = true;
    auto snippet = build_snippet(ccs);
    if(snippet.empty()) {
        item.insert = std::move(complete);
    } else if(options.arguments == CompletionArguments::Placeholders) {
        item.insert = std::move(snippet);
        item.snippet = true;
    } else {
        item.insert = item.bundled_insert;
        item.snippet = true;
    }
}

bool Reply::expected(const clang::NamedDecl* decl) const {
    auto preferred = context.getPreferredType();
    if(preferred.isNull() && completing.expected_type == 0) {
        return false;
    }
    auto& ast = sema.getASTContext();
    clang::QualType type;
    if(auto* value = llvm::dyn_cast<clang::ValueDecl>(decl)) {
        type = value->getType();
    } else if(auto* function = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
        type = function->getTemplatedDecl()->getType();
    } else if(auto* named = llvm::dyn_cast<clang::TypeDecl>(decl)) {
        type = ast.getTypeDeclType(named);
    }
    if(auto* function = type.isNull() ? nullptr : type->getAs<clang::FunctionType>()) {
        type = function->getReturnType();
    }
    if(type.isNull()) {
        return false;
    }
    type = type.getNonReferenceType();
    if(preferred.isNull()) {
        return completing.unit.get<EntityTable>().type_hash(type.getUnqualifiedType()) ==
               completing.expected_type;
    }
    return ast.hasSameUnqualifiedType(type, preferred.getNonReferenceType());
}

void Reply::add_overrides() {
    const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(sema.CurContext);
    if(!record || context.getKind() != clang::CodeCompletionContext::CCC_ClassStructUnion ||
       !sema.getCurScope()->isClassScope()) {
        return;
    }
    auto policy = completion_policy(sema);
    for(const auto* method: overridable(*record)) {
        clang::CodeCompletionBuilder builder(completing.info.getAllocator(), completing.info);
        auto& pattern = *clang::CodeCompletionResult(method, clang::CCP_CodePattern)
                             .createCodeCompletionStringForOverride(sema.getPreprocessor(),
                                                                    sema.getASTContext(),
                                                                    builder,
                                                                    /*IncludeBriefComments=*/false,
                                                                    context,
                                                                    policy);
        add_item({
            .label = pattern.getAllTypedText(),
            .kind = protocol::CompletionItemKind::Method,
            .insert = pattern_text(pattern, /*whole=*/false) + ";",
            .relevance = priority_weight(clang::CCP_CodePattern),
        });
    }
}

void Reply::add_import() {
    auto* module = sema.getCurrentModule();
    if(context.getKind() != clang::CodeCompletionContext::CCC_TopLevel ||
       !sema.CurContext->isTranslationUnit() || !imports || !module ||
       (module->Kind != clang::Module::ModuleImplementationUnit &&
        module->Kind != clang::Module::ModulePartitionImplementation)) {
        return;
    }
    clang::CodeCompletionBuilder builder(completing.info.getAllocator(), completing.info);
    builder.AddTypedTextChunk("import");
    builder.AddChunk(clang::CodeCompletionString::CK_HorizontalSpace);
    builder.AddPlaceholderChunk("name");
    builder.AddChunk(clang::CodeCompletionString::CK_SemiColon);
    builder.AddChunk(clang::CodeCompletionString::CK_VerticalSpace);
    add_keyword_pattern(*builder.TakeString(), clang::CCP_CodePattern);
}

void Reply::render_kept() {
    // In bundle mode one item of a label is shown (dedup_priority),
    // the first added among equals: a declaration an item added
    // already displaces is no candidate.
    llvm::StringMap<int> displacing;
    if(completing.options.bundle_overloads) {
        for(auto& entry: items) {
            if(entry.item.kind != protocol::CompletionItemKind::Snippet) {
                auto [it, inserted] = displacing.try_emplace(entry.item.label, entry.priority);
                it->second = std::max(it->second, entry.priority);
            }
        }
    }
    llvm::StringMap<std::pair<float, std::optional<std::uint32_t>>> best;
    for(auto& each: pending) {
        if(auto it = displacing.find(each.item.label);
           it != displacing.end() &&
           it->second >= dedup_priority(each.item.kind, each.item.macro)) {
            continue;
        }
        auto [it, inserted] = best.try_emplace(each.item.label, each.score, each.item.order);
        if(!inserted && each.score > it->second.first) {
            it->second = {each.score, std::nullopt};
        }
    }
    std::vector<std::pair<std::string, llvm::StringRef>> ranked;
    ranked.reserve(best.size());
    for(auto& entry: best) {
        ranked.emplace_back(sort_text(entry.second.first, entry.second.second), entry.getKey());
    }
    llvm::StringSet<> kept;
    auto keep = std::min<std::size_t>(ranked.size(), at_most(completing.options.limit));
    completing.more_names = completing.more_names || keep < ranked.size();
    ranges::partial_sort(ranked, ranked.begin() + keep);
    for(auto& [order, label]: views::take(ranked, keep)) {
        kept.insert(label);
    }
    for(auto& each: pending) {
        if(kept.contains(each.item.label)) {
            render(each.candidate, std::move(each.item));
        }
    }
}

auto Reply::finish() -> std::vector<Item> {
    render_kept();

    for(auto& overloads: bundles | views::values) {
        if(overloads.count > 1) {
            overloads.item.label_details = bundle_details(overloads.count);
            if(!overloads.bundled_insert.empty()) {
                std::visit(
                    [&](auto& replaced) {
                        replaced.new_text = std::move(overloads.bundled_insert);
                    },
                    *overloads.item.text_edit);
                if(overloads.bundled_snippet) {
                    overloads.item.insert_text_format = protocol::InsertTextFormat::Snippet;
                }
            }
        }
        items.push_back({
            .item = std::move(overloads.item),
            .priority = overloads.priority,
            .documented = overloads.documented,
        });
    }

    // In bundle mode, deduplicate same-label candidates (see dedup_priority).
    if(completing.options.bundle_overloads) {
        std::unordered_map<std::string, std::size_t> label_index;
        std::vector<Listed> deduped;
        deduped.reserve(items.size());

        for(auto& entry: items) {
            // The variants of one statement share their keyword, the
            // selectors of one first piece their label.
            if(entry.item.kind == protocol::CompletionItemKind::Snippet ||
               llvm::isa_and_present<clang::ObjCMethodDecl>(entry.documented)) {
                deduped.push_back(std::move(entry));
                continue;
            }
            auto [it, inserted] = label_index.try_emplace(entry.item.label, deduped.size());
            if(inserted) {
                deduped.push_back(std::move(entry));
            } else if(entry.priority > deduped[it->second].priority) {
                deduped[it->second] = std::move(entry);
            }
        }
        items.swap(deduped);
    }

    return items | views::transform([](Listed& entry) {
               return Item{.item = std::move(entry.item), .documented = entry.documented};
           }) |
           ranges::to<std::vector>();
}

}  // namespace clice::feature
