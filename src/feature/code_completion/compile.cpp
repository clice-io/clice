module clice;

import :compile.compilation;
import :feature.code_actions.action;
import :feature.code_actions;
import :feature.code_completion.cursor;
import :feature.code_completion.typed_names;
import :feature.code_completion;
import :feature.documentation;
import :index.query.locate;
import :semantic.comment;
import :semantic.decls;
import :semantic.display;
import :semantic.identity;
import :semantic.template_resolver;
import :support.fuzzy_matcher;
import :support.markup;
import :syntax.lexer;

namespace clice::feature {

namespace {

/// The `import` declaration a unit of the module `unit` (`name` or
/// `name:partition`, empty outside a named module) needs to reach what the
/// module `module` exports; none when the unit is that module. The compile
/// adds an item's `import` itself, while its `#include` waits for the
/// server (finish_completion).
auto import_for(llvm::StringRef module, llvm::StringRef unit) -> std::optional<std::string> {
    auto [primary, partition] = module.split(':');
    if(primary != unit.split(':').first) {
        return std::format("import {};\n", primary);
    }
    if(!partition.empty() && module != unit) {
        return std::format("import :{};\n", partition);
    }
    return std::nullopt;
}

/// The dependent expression at the completion point, in the innermost
/// function holding it (AtPoint).
struct AtPointFinder : clang::RecursiveASTVisitor<AtPointFinder> {
    AtPointFinder(CompilationUnit& unit, std::uint32_t target) : unit(unit), target(target) {}

    bool at(clang::SourceLocation loc) {
        return unit.focus_file().offset(loc) == target;
    }

    bool holds(const clang::Stmt* body) {
        if(!body) {
            return false;
        }
        auto begin = unit.focus_file().offset(body->getBeginLoc());
        return begin && *begin <= target && target <= unit.seen_at(body->getEndLoc()).offset;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* function) {
        if(holds(function->getBody())) {
            context = function;
        }
        return true;
    }

    bool VisitLambdaExpr(clang::LambdaExpr* lambda) {
        if(holds(lambda->getBody())) {
            context = lambda->getCallOperator();
        }
        return true;
    }

    bool VisitIfStmt(clang::IfStmt* branch) {
        if(branch->isConstexpr() && (holds(branch->getThen()) || holds(branch->getElse()))) {
            constexpr_branch = true;
        }
        return true;
    }

    bool VisitCXXDependentScopeMemberExpr(clang::CXXDependentScopeMemberExpr* access) {
        if(at(access->getMemberLoc())) {
            member = access;
        }
        return !found();
    }

    bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* name) {
        if(at(name->getLocation())) {
            qualifier = name->getQualifierLoc();
        }
        return !found();
    }

    bool VisitDependentNameTypeLoc(clang::DependentNameTypeLoc name) {
        if(at(name.getNameLoc())) {
            qualifier = name.getQualifierLoc();
        }
        return !found();
    }

    bool found() const {
        return member || qualifier;
    }

    CompilationUnit& unit;

    std::uint32_t target;

    clang::FunctionDecl* context = nullptr;

    clang::CXXDependentScopeMemberExpr* member = nullptr;

    clang::NestedNameSpecifierLoc qualifier;

    bool constexpr_branch = false;
};

/// The dependent expression at the completion point as the whole buffer's
/// compile parses it, in the innermost function holding it.
struct AtPoint {
    CompilationUnit& unit;

    clang::FunctionDecl* context = nullptr;

    clang::CXXDependentScopeMemberExpr* member = nullptr;

    /// Of a name a dependent scope qualifies, in an expression or a type.
    clang::NestedNameSpecifierLoc qualifier;

    /// The point is in a branch of an `if constexpr`, which an instantiation
    /// may discard.
    bool constexpr_branch = false;
};

/// Whether `instantiation` discards the code at `loc`, a branch of an
/// `if constexpr` it does not take, which its body keeps empty. A body Sema
/// deferred is instantiated here.
static bool discards(clang::Sema& sema,
                     clang::FunctionDecl& instantiation,
                     clang::SourceLocation loc) {
    sema.InstantiateFunctionDefinition(instantiation.getPointOfInstantiation(), &instantiation);
    auto& sources = sema.getSourceManager();
    auto discarded = [&](this auto& self, const clang::Stmt* stmt) -> bool {
        if(!stmt) {
            return false;
        }
        if(auto* branch = llvm::dyn_cast<clang::IfStmt>(stmt); branch && branch->isConstexpr()) {
            for(const auto* taken: {branch->getThen(), branch->getElse()}) {
                auto* block = llvm::dyn_cast_or_null<clang::CompoundStmt>(taken);
                if(block && block->body_empty() &&
                   sources.isPointWithin(loc, block->getBeginLoc(), block->getEndLoc())) {
                    return true;
                }
            }
        }
        for(const auto* child: stmt->children()) {
            if(self(child)) {
                return true;
            }
        }
        return false;
    };
    return discarded(instantiation.getBody());
}

/// A second compile of the whole buffer a completion runs on, for what only
/// the code after the completion point tells. Its parse reads a name at the
/// point, where there may be none yet, so that the expression there parses
/// whole. Run at most once, when asked.
class WholeBuffer {
public:
    explicit WholeBuffer(const CompilationParams& completion);

    /// The expression at the point, the template `templated` around it
    /// instantiated as the code after the point does: the compile skips the
    /// focus file's function bodies but those that may instantiate it, and
    /// stops once calls instantiated it CompilationParams::Instantiating::limit
    /// times.
    /// For the variable of a range-based `for`, `loop`, the statement there
    /// alone tells. None when nothing after the point calls the template, as
    /// `completion` has the macros before it, when the compile did not
    /// complete and when nothing dependent stands at the point.
    auto instantiated(const clang::NamedDecl* templated, bool loop, CompilationUnit& completion)
        -> std::optional<AtPoint>;

    /// The compile once it completed; null before.
    CompilationUnit* unit();

private:
    /// Whether the code after the point spells `name`, or a macro whose
    /// expansion does.
    bool called_after(llvm::StringRef name, clang::Preprocessor& pp);

private:
    CompilationInput input;

    std::uint32_t point;

    /// Where the name its parse reads at the point starts.
    std::uint32_t target = 0;

    std::shared_ptr<std::atomic_bool> stop;

    std::unique_ptr<CompilationUnit> compiled;
};

WholeBuffer::WholeBuffer(const CompilationParams& completion) :
    input(completion.input), point(completion.completion.offset), stop(completion.stop) {
    auto& text = input.buffers.find(input.focus)->second;
    auto whole = Prefix::from(text, point).whole;
    target = whole.begin;
    if(whole.length() == 0) {
        text.insert(point, " __clice_completion");
        target = point + 1;
    }
}

auto WholeBuffer::instantiated(const clang::NamedDecl* templated,
                               bool loop,
                               CompilationUnit& completion) -> std::optional<AtPoint> {
    if(!compiled) {
        // The resolver binds no class template's parameters through its
        // instantiations: a member of one learns nothing more here, but for
        // the statement at the point.
        bool member = llvm::isa<clang::CXXRecordDecl>(templated);
        if(member && !loop) {
            return std::nullopt;
        }
        // A call after the point names the template it calls, a constructor
        // its class; one nothing calls yet goes without the compile. A lambda
        // in a function is called in the body holding the point; any body may
        // call an operator, a conversion or a lambda elsewhere.
        std::optional<llvm::StringSet<>> names;
        const auto* named = templated;
        if(auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(templated)) {
            named = constructor->getParent();
        }
        auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(templated);
        if(auto* name = named->getIdentifier()) {
            if(!loop && !called_after(name->getName(), completion.sema().getPreprocessor())) {
                return std::nullopt;
            }
            names.emplace().insert(name->getName());
        } else if(method && method->getParent()->isLambda() &&
                  method->getParent()->getParentFunctionOrMethod()) {
            names.emplace();
        }
        std::optional<std::uint32_t> declared;
        if(!member) {
            declared = completion.produced_at(templated->getLocation()).offset;
        }
        compiled = compile({
            .kind = CompilationKind::Content,
            .input = input,
            .instantiating =
                CompilationParams::Instantiating{
                                                 .point = point,
                                                 .names = std::move(names),
                                                 .templated = declared,
                                                 },
            .stop = stop,
        });
    }
    auto* whole = unit();
    if(!whole) {
        return std::nullopt;
    }
    AtPointFinder finder(*whole, target);
    for(auto* decl: whole->top_level_decls()) {
        if(!finder.TraverseDecl(decl)) {
            break;
        }
    }
    if(!finder.found() || !finder.context) {
        return std::nullopt;
    }
    return AtPoint{
        .unit = *whole,
        .context = finder.context,
        .member = finder.member,
        .qualifier = finder.qualifier,
        .constexpr_branch = finder.constexpr_branch,
    };
}

bool WholeBuffer::called_after(llvm::StringRef name, clang::Preprocessor& pp) {
    auto& text = input.buffers.find(input.focus)->second;
    clang::Lexer lexer({},
                       pp.getLangOpts(),
                       text.data(),
                       text.data() + point,
                       text.data() + text.size());
    auto spells = [&](llvm::StringRef word) {
        return word == name;
    };
    llvm::DenseMap<const clang::MacroInfo*, bool> read;
    clang::Token token;
    for(bool end = false; !end;) {
        end = lexer.LexFromRawLexer(token);
        if(token.is(clang::tok::raw_identifier) &&
           (spells(token.getRawIdentifier()) ||
            macro_spells(pp, token.getRawIdentifier(), spells, read))) {
            return true;
        }
    }
    return false;
}

CompilationUnit* WholeBuffer::unit() {
    return compiled && compiled->completed() ? compiled.get() : nullptr;
}

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

/// The namespace the qualifier of the name being completed names, outermost
/// first, inline and anonymous ones left out: as Sema resolves it, or as the
/// text spells it while no declaration of it is visible (`std::` before any
/// standard header). None for an unqualified name, and one a class
/// qualifies.
std::optional<std::vector<std::string>> qualifying_namespace(clang::Sema& sema,
                                                             clang::CodeCompletionContext& context,
                                                             llvm::StringRef content,
                                                             std::uint32_t name) {
    auto spec = context.getCXXScopeSpecifier();
    if(!spec || (*spec)->isEmpty()) {
        return std::nullopt;
    }
    if((*spec)->isInvalid()) {
        auto written = written_scope(content, name, sema.getLangOpts());
        if(!written) {
            return std::nullopt;
        }
        return written->segments | views::transform(&llvm::StringRef::str) |
               ranges::to<std::vector>();
    }
    auto* scope = sema.computeDeclContext(**spec);
    if(!scope || !scope->isFileContext()) {
        return std::nullopt;
    }
    std::vector<std::string> segments;
    for(; !scope->isTranslationUnit(); scope = scope->getParent()) {
        if(auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
           space && !space->isInline() && !space->isAnonymousNamespace()) {
            segments.push_back(space->getName().str());
        }
    }
    ranges::reverse(segments);
    return segments;
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

/// The label details of an item standing for `count` overloads of a name.
protocol::CompletionItemLabelDetails bundle_details(std::size_t count) {
    protocol::CompletionItemLabelDetails details;
    details.detail = std::format("(…) +{} overload{}", count - 1, count > 2 ? "s" : "");
    return details;
}

/// Make `item`, one overload of a name, stand for all `count` of them: a
/// call it inserts takes its parentheses with the cursor between them in
/// place of its placeholders.
void bundle(protocol::CompletionItem& item, std::size_t count) {
    item.label_details = bundle_details(count);
    if(item.insert_text_format != protocol::InsertTextFormat::Snippet) {
        return;
    }
    std::visit(
        [&](auto& edit) {
            if(llvm::StringRef(edit.new_text).starts_with(item.label + "(")) {
                edit.new_text = escape_snippet(item.label) + "($0)";
            }
        },
        *item.text_edit);
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
    static clang::CodeCompleteOptions sema_options(const CodeCompletionOptions& options) {
        clang::CodeCompleteOptions sema;
        sema.IncludeMacros = 1;
        sema.IncludeCodePatterns = 1;
        sema.IncludeFixIts = options.dot_to_arrow;
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
            WholeBuffer* whole_buffer,
            bool& includes_pending,
            bool& more_names) :
        clang::CodeCompleteConsumer(sema_options(options)), unit(unit), focus(unit.focus_file()),
        text(focus.content()), offset(offset), encoding(encoding), output(output),
        documented(documented), options(options), client(client), names(names),
        whole_buffer(whole_buffer), includes_pending(includes_pending), more_names(more_names),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    clang::CodeCompletionAllocator& getAllocator() final {
        // Sema asks first thing in a completion, before it reads any name.
        // A nested run completes in the whole buffer's compile, whose files
        // it reads whole.
        if(whole_buffer && !typed_names_open) {
            typed_names_open = true;
            if(auto typed = Prefix::from(text, offset).spelling; !typed.empty()) {
                typed_names = TypedNames::open(unit.sema(), typed);
            }
        }
        // A tentative parse reaching the point mostly runs a completion of
        // its recovery, whose names ProcessCodeCompleteResults drops: with
        // nothing typed, Sema would read every name the loaded files
        // declare for it. Sema asks the consumer whether to read them after
        // it asks for the allocator, so that completion runs through
        // `unloaded`, which reads none.
        auto& completion = unit.sema().CodeCompletion();
        if(whole_buffer && !typed_names && !processing && completion.CodeCompleter == this &&
           unit.sema().getPreprocessor().isBacktrackEnabled()) {
            completion.CodeCompleter = &unloaded;
        }
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

    /// What a nested run may list the members of. A concrete specialization
    /// is instantiated first, as a written `X<int> x; x.` would be; one that
    /// does not instantiate, or a pattern without a definition, has nothing
    /// to list.
    static clang::TagDecl* definition(clang::Sema& sema,
                                      clang::SourceLocation loc,
                                      clang::TagDecl* tag) {
        auto& ast = sema.getASTContext();
        if(!tag->isDependentContext() && !sema.isCompleteType(loc, ast.getCanonicalTagType(tag))) {
            return nullptr;
        }
        return tag->getDefinition();
    }

    /// Sema's reply is replaced once the nested run `complete` offers
    /// candidates, even when the typed prefix then filters every one of
    /// them out; a nested run offering none keeps what an earlier callback
    /// left in `output`.
    bool replace(llvm::function_ref<void()> complete) {
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
    }

    /// The members of an object of `record`, qualified as `object` is.
    bool complete_members(clang::Sema& sema,
                          clang::Scope* scope,
                          clang::SourceLocation loc,
                          clang::CXXRecordDecl* record,
                          clang::QualType object,
                          clang::QualType preferred) {
        auto& ast = sema.getASTContext();
        auto type = ast.getQualifiedType(ast.getCanonicalTagType(record), object.getQualifiers());
        auto* stand_in = new (ast) clang::OpaqueValueExpr(loc, type, clang::VK_LValue);
        return replace([&] {
            sema.CodeCompletion().CodeCompleteMemberReferenceExpr(scope,
                                                                  stand_in,
                                                                  /*OtherOpBase=*/nullptr,
                                                                  loc,
                                                                  /*IsArrow=*/false,
                                                                  /*IsBaseExprStatement=*/false,
                                                                  preferred);
        });
    }

    /// The names `tag` scopes, as after `tag::` written at `range`.
    bool complete_scope(clang::Sema& sema,
                        clang::Scope* scope,
                        clang::TagDecl* tag,
                        clang::SourceRange range,
                        clang::CodeCompletionContext context) {
        auto& ast = sema.getASTContext();
        /// Sema computes no declaration context for an enumeration inside a
        /// template; offer its enumerators directly.
        if(auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(tag)) {
            std::vector<clang::CodeCompletionResult> enumerators;
            for(auto* enumerator: enumeration->enumerators()) {
                enumerators.emplace_back(enumerator, clang::CCP_Constant);
            }
            return replace([&] {
                ProcessCodeCompleteResults(sema, context, enumerators.data(), enumerators.size());
            });
        }
        clang::CXXScopeSpec resolved;
        resolved.MakeTrivial(ast,
                             clang::NestedNameSpecifier(ast.getCanonicalTagType(tag).getTypePtr()),
                             range);
        return replace([&] {
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

    /// The template around `context` the code after the point may
    /// instantiate: a function template, a generic lambda among them, or a
    /// class template or partial specialization a member function is in.
    const static clang::NamedDecl* around_template(const clang::DeclContext* context) {
        if(const auto* function = enclosing_template(context)) {
            return function;
        }
        for(; context; context = context->getParent()) {
            if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
               record && (record->getDescribedClassTemplate() ||
                          llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(record))) {
                return record;
            }
        }
        return nullptr;
    }

    /// Completes against what the code after the point instantiates
    /// `templated` with, as the compile of the whole buffer (WholeBuffer)
    /// sees it: the member access or the scope at the point found there,
    /// resolved through an instantiation of the template, and completed by
    /// its Sema. A function template instantiated more than once lists the
    /// names every instantiation has, of at most
    /// CompilationParams::Instantiating::limit of those that do not discard
    /// the point. Whether that produced the reply. For the variable of a
    /// range-based `for` it is the statement there that tells, which the
    /// completion's own parse leaves unfinished.
    bool complete_instantiated(const clang::NamedDecl* templated,
                               bool loop,
                               clang::QualType preferred) {
        auto found = whole_buffer->instantiated(templated, loop, unit);
        if(!found) {
            return false;
        }
        auto* pattern = enclosing_template(found->context);
        auto calls = pattern ? decls::instantiations(*pattern->getDescribedFunctionTemplate())
                             : llvm::SmallVector<clang::FunctionDecl*, 4>();
        // LIMITATION: a generic lambda inside a function template is never
        // instantiated itself, only the copies in the template's
        // instantiations are: in its body, a name of the template's
        // parameters completes through the template's only instantiation, and
        // to nothing when the template has several.
        if(calls.size() < 2) {
            return complete_through(*found, nullptr, preferred, options, output, documented);
        }
        if(found->constexpr_branch) {
            auto loc = found->member ? found->member->getMemberLoc() : found->qualifier.getEndLoc();
            llvm::erase_if(calls, [&](clang::FunctionDecl* call) {
                return discards(found->unit.sema(), *call, loc);
            });
        }
        if(calls.empty()) {
            return false;
        }
        calls.truncate(
            std::min<std::size_t>(calls.size(), CompilationParams::Instantiating::limit));
        // Each instantiation lists all its names, each overload apart: the
        // reply's limit cuts the names they share, and one bundle holds the
        // overloads of a name every instantiation declares.
        auto every = options;
        every.limit = 0;
        every.bundle_overloads = false;
        auto name_of = [](const protocol::CompletionItem& item) -> llvm::StringRef {
            return item.filter_text ? *item.filter_text : item.label;
        };
        std::vector<protocol::CompletionItem> items;
        std::vector<const clang::NamedDecl*> described;
        std::set<std::pair<std::string, std::string>> listed;
        llvm::StringMap<std::size_t> having;
        for(auto* call: calls) {
            std::vector<protocol::CompletionItem> offered;
            std::vector<const clang::NamedDecl*> offered_described;
            if(!complete_through(*found, call, preferred, every, offered, offered_described)) {
                return false;
            }
            llvm::StringSet<> names;
            for(auto&& [item, decl]: views::zip(offered, offered_described)) {
                names.insert(name_of(item));
                auto& details = item.label_details;
                auto overload = details && details->detail ? *details->detail : std::string();
                if(listed.emplace(item.label, std::move(overload)).second) {
                    items.push_back(std::move(item));
                    described.push_back(decl);
                }
            }
            for(auto& name: names) {
                having[name.getKey()] += 1;
            }
        }
        output.clear();
        documented.clear();
        // The overloads in each bundle, by the item standing for it.
        llvm::StringMap<std::size_t> bundles;
        std::vector<std::size_t> overloads;
        for(auto&& [item, decl]: views::zip(items, described)) {
            if(having.lookup(name_of(item)) != calls.size()) {
                continue;
            }
            using enum protocol::CompletionItemKind;
            if(options.bundle_overloads &&
               (item.kind == Function || item.kind == Method || item.kind == Constructor)) {
                auto [it, inserted] = bundles.try_emplace(item.label, output.size());
                if(!inserted) {
                    overloads[it->second] += 1;
                    output[it->second].sort_text =
                        std::min(*output[it->second].sort_text, *item.sort_text);
                    continue;
                }
            }
            output.push_back(std::move(item));
            documented.push_back(decl);
            overloads.push_back(1);
        }
        for(auto&& [item, count]: views::zip(output, overloads)) {
            if(count > 1) {
                bundle(item, count);
            }
        }
        return true;
    }

    /// One completion of complete_instantiated at `found`, the parameters of
    /// the template around it bound to `instantiation`'s arguments, or to
    /// its only instantiation's without one, into `items`.
    bool complete_through(const AtPoint& found,
                          const clang::FunctionDecl* instantiation,
                          clang::QualType preferred,
                          const CodeCompletionOptions& run_options,
                          std::vector<protocol::CompletionItem>& items,
                          std::vector<const clang::NamedDecl*>& described) {
        auto& other = found.unit.sema();
        auto& ast = found.unit.context();

        const static CompileCompletion none;
        bool includes = false;
        bool more = false;
        Builder nested(found.unit,
                       offset,
                       encoding,
                       items,
                       described,
                       run_options,
                       client,
                       none,
                       nullptr,
                       includes,
                       more);
        nested.text = text;
        if(!preferred.isNull() && !preferred->isDependentType()) {
            nested.expected_type = unit.get<EntityTable>().type_hash(
                preferred.getNonReferenceType().getUnqualifiedType());
        }
        auto& completion = other.CodeCompletion();
        auto* consumer = completion.CodeCompleter;
        completion.CodeCompleter = &nested;
        clang::Sema::ContextRAII entered(other, found.context);
        // Where the parse stood, as far as Sema asks: whether the context is
        // a template.
        clang::Scope scope(nullptr,
                           clang::Scope::FnScope | clang::Scope::DeclScope,
                           other.getDiagnostics());
        scope.setEntity(found.context);
        types::TemplateResolver resolver(ast, {.instantiation = instantiation});
        bool answered = false;
        if(auto* access = found.member) {
            auto loc = access->getMemberLoc();
            auto object = resolver.member_object(access->getBase(), access->isArrow());
            auto* tag = object.isNull() ? nullptr : resolver.resolve_tag(object);
            nested.display_options.specialization =
                object.isNull() ? nullptr : object->getAs<clang::TemplateSpecializationType>();
            if(auto* record =
                   tag ? llvm::dyn_cast_or_null<clang::CXXRecordDecl>(definition(other, loc, tag))
                       : nullptr) {
                answered = nested.complete_members(other, &scope, loc, record, object, {});
            }
        } else if(auto qualifier = found.qualifier.getNestedNameSpecifier();
                  qualifier.getKind() == clang::NestedNameSpecifier::Kind::Type) {
            auto range = found.qualifier.getSourceRange();
            auto* tag = resolver.resolve_tag(clang::QualType(qualifier.getAsType(), 0));
            tag = tag ? definition(other, range.getEnd(), tag) : nullptr;
            if(tag) {
                answered = nested.complete_scope(
                    other,
                    &scope,
                    tag,
                    range,
                    clang::CodeCompletionContext(clang::CodeCompletionContext::CCC_Symbol));
            }
        }
        completion.CodeCompleter = consumer;
        more_names = more_names || (answered && more);
        return answered;
    }

    /// Sema resolves a dependent member base or scope with its own
    /// heuristics, which stop at the primary template and a single typedef.
    /// When pseudo-instantiation reaches a class Sema did not, run the
    /// completion again against that class; when it reaches none in a
    /// template, against what the code after the point instantiates the
    /// template with. Returns whether the nested run produced the reply,
    /// which is then in `output`.
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
                // LIMITATION: only the specializations declared before the
                // point count: a class that one declared after it may replace
                // lists the primary template's members.
                /// A class another specialization may replace once the
                /// template is instantiated is the instantiation's to tell.
                if(auto* templated = around_template(sema.CurContext);
                   templated && (!tag || resolver.specializable(object))) {
                    auto* name = llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
                    auto* var = name ? llvm::dyn_cast<clang::VarDecl>(name->getDecl()) : nullptr;
                    if(complete_instantiated(templated,
                                             var && var->isCXXForRangeDecl(),
                                             context.getPreferredType())) {
                        return true;
                    }
                    if(!tag) {
                        return false;
                    }
                }
                /// The members of a dependent specialization read its
                /// template's parameters as its arguments.
                display_options.specialization =
                    object.isNull() ? nullptr : object->getAs<clang::TemplateSpecializationType>();
            } else if(!reported->isDependentType()) {
                return false;
            } else {
                tag = resolver.resolve_tag(object);
            }
            if(!tag || same(tag, clang::HeuristicResolver(ast).resolveTypeToTagDecl(reported))) {
                return false;
            }
            auto* record = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(definition(sema, loc, tag));
            if(!record) {
                return false;
            }

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
                auto type =
                    ast.getQualifiedType(ast.getCanonicalTagType(record), object.getQualifiers());
                return replace([&] {
                    sema.CodeCompletion().CodeCompleteDesignator(type, {}, clang::Designation());
                });
            }
            return complete_members(sema, scope, loc, record, object, context.getPreferredType());
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
            if(auto* templated = around_template(sema.CurContext);
               templated && (!tag || resolver.specializable(scope_type))) {
                if(complete_instantiated(templated, false, context.getPreferredType())) {
                    return true;
                }
                if(!tag) {
                    return false;
                }
            }
            /// The members of a dependent specialization read its
            /// template's parameters as its arguments.
            display_options.specialization =
                resolver.resolve(scope_type)->getAs<clang::TemplateSpecializationType>();
            auto* entered = llvm::dyn_cast_or_null<clang::TagDecl>(
                sema.computeDeclContext(**spec, /*EnteringContext=*/true));
            if(!tag || same(tag, entered)) {
                return false;
            }
            tag = definition(sema, loc, tag);
            if(!tag) {
                return false;
            }
            return complete_scope(sema, scope, tag, (*spec)->getRange(), context);
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
        bool unloaded_reply = std::exchange(unloaded_run, false);
        if(context.getKind() == clang::CodeCompletionContext::CCC_Recovery ||
           (lexer && lexer->isLexingRawMode())) {
            return;
        }
        auto processed = llvm::make_scope_exit([&, was = processing] { processing = was; });
        processing = true;
        // A tentative parse that keeps what it parsed at the point
        // (`requires { typename lib:: }`) made the completion that stands
        // through `unloaded`: the names of a namespace, or of the global one,
        // qualifying it come again, read.
        if(unloaded_reply && !resolving) {
            using enum clang::NestedNameSpecifier::Kind;
            if(auto spec = context.getCXXScopeSpecifier();
               spec && ((*spec)->getScopeRep().getKind() == Namespace ||
                        (*spec)->getScopeRep().getKind() == Global)) {
                auto qualifier = **spec;
                if(replace([&] {
                       sema.CodeCompletion().CodeCompleteQualifiedId(
                           sema.getCurScope(),
                           qualifier,
                           /*EnteringContext=*/false,
                           context.isUsingDeclaration(),
                           /*IsAddressOfOperand=*/false,
                           /*IsInDeclarationContext=*/false,
                           /*BaseType=*/{},
                           context.getPreferredType());
                   })) {
                    return;
                }
            }
        }

        if(resolving) {
            resolved_candidates |= candidate_count > 0;
        } else {
            display_options.specialization = nullptr;
            if(complete_resolved(sema, context)) {
                return;
            }
            more_names = typed_names && typed_names->more();
        }

        if(candidate_count == 0 && names.candidates.empty()) {
            return;
        }

        auto content = text;
        auto prefix = Prefix::from(content, offset);
        FuzzyMatcher matcher(prefix.spelling);

        auto map = focus.positions();
        auto typed = map.range(prefix.typed, encoding);
        auto whole = map.range(prefix.whole, encoding);
        if(!typed || !whole) {
            return;
        }
        // Call or template arguments already written after the name: a `<`
        // opens a template's, and compares anything else.
        auto after = content.drop_front(prefix.whole.end).ltrim();
        bool call_follows = after.starts_with("(");
        bool angles_follow = after.starts_with("<");
        bool colon_follows = after.starts_with(":");
        auto arguments_follow = [&](bool templated) {
            return call_follows || (angles_follow && templated);
        };

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

        /// Sema offers an Objective-C method as a pattern where it is written
        /// whole: its declaration in an implementation, after the `-` or the
        /// return type, its body left to write; after a dot one taking no
        /// argument, read as a property; and in a message to `super` the
        /// method being written, passed its arguments — in place of the
        /// method's own candidate.
        auto add_method_pattern = [&](const clang::CodeCompletionString& pattern,
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
            item.insert = colon_follows ? llvm::StringRef(item.label).rtrim(':').str()
                                        : llvm::StringRef(item.insert).rtrim().str();
            item.signature = llvm::StringRef(item.signature).rtrim().str();
            if(auto typed = pattern.getAllTypedText(); typed != item.label) {
                item.filter = std::move(typed);
            }
            add(item);
        };

        // In a module unit, imports precede every other declaration of its
        // purview, none in its global module fragment, and only an interface
        // exports; elsewhere `export` only begins one, before any
        // declaration. Clang's keywords know neither.
        auto* module = sema.getCurrentModule();
        bool named_module = module && module->isNamedModule();
        auto completion_loc = focus.location(offset);
        auto declared = ranges::any_of(
            sema.getASTContext().getTranslationUnitDecl()->noload_decls(),
            [&](const clang::Decl* decl) {
                return !decl->isImplicit() && !llvm::isa<clang::ImportDecl>(decl) &&
                       (named_module ? decl->isInNamedModule()
                                     : focus.written_offset(decl->getLocation()).has_value()) &&
                       unit.before(decl->getLocation(), completion_loc);
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
        auto insert_call =
            [&](Candidate& item, const clang::CodeCompletionString& ccs, bool called_here) {
                if(arguments_follow(llvm::isa_and_present<clang::TemplateDecl>(item.documented)) ||
                   !called_here || options.arguments == CompletionArguments::None) {
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
                } else if(options.arguments == CompletionArguments::Placeholders) {
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

        /// Whether `decl` is, names, or a call of it returns, the type the
        /// context expects.
        auto expected = [&, preferred = context.getPreferredType()](const clang::NamedDecl* decl) {
            if(preferred.isNull() && expected_type == 0) {
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
                return unit.get<EntityTable>().type_hash(type.getUnqualifiedType()) ==
                       expected_type;
            }
            return ast.hasSameUnqualifiedType(type, preferred.getNonReferenceType());
        };

        llvm::DenseSet<const clang::Decl*> offered;
        /// After `struct`, `union` or `enum`, where Sema offers only tags.
        bool tags_only =
            ranges::contains(std::array{clang::CodeCompletionContext::CCC_ClassOrStructTag,
                                        clang::CodeCompletionContext::CCC_UnionTag,
                                        clang::CodeCompletionContext::CCC_EnumTag},
                             context.getKind());
        /// A declaration's item past its name: what costs most, its
        /// signature and types, spent on those the list keeps alone.
        auto render_declaration = [&](clang::CodeCompletionResult& candidate, Candidate item) {
            auto* declaration = candidate.Declaration;
            auto kind = item.kind;
            bool is_callable = kind == protocol::CompletionItemKind::Function ||
                               kind == protocol::CompletionItemKind::Method ||
                               kind == protocol::CompletionItemKind::Constructor;
            clang::CodeCompletionBuilder builder(getAllocator(), getCodeCompletionTUInfo());
            const auto* pattern = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration);
            auto& ccs = pattern && !candidate.StartsNestedNameSpecifier
                            ? class_template_string(builder, candidate, *pattern, sema)
                            : *candidate.CreateCodeCompletionString(sema,
                                                                    context,
                                                                    getAllocator(),
                                                                    getCodeCompletionTUInfo(),
                                                                    /*IncludeBriefComments=*/false);
            item.return_type = extract_return_type(ccs);
            if(llvm::isa<clang::ObjCMethodDecl>(declaration)) {
                item.signature = pattern_tail(ccs);
                if(candidate.DeclaringEntity) {
                    // A method's declaration after its return type.
                    item.insert = pattern_text(ccs, /*whole=*/true);
                } else if(colon_follows) {
                    item.insert = llvm::StringRef(item.label).rtrim(':').str();
                } else if(client.snippets && options.arguments != CompletionArguments::None) {
                    auto snippet =
                        build_snippet(ccs, options.arguments == CompletionArguments::Placeholders);
                    if(!snippet.empty()) {
                        item.insert = std::move(snippet);
                        item.snippet = true;
                    }
                }
                add(item);
                return;
            }
            // Clang gives a variable holding a lambda the signature of its
            // call operator, whose `const` is no part of calling it.
            bool method = llvm::isa_and_present<clang::CXXMethodDecl>(declaration->getAsFunction());
            std::size_t parameters;
            item.signature = extract_signature(ccs, /*qualifiers=*/method, &parameters);
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

            add(item);
        };

        /// The declarations matched, of which those the list keeps are
        /// rendered once all are known (code_completion.limit).
        struct Pending {
            clang::CodeCompletionResult candidate;
            Candidate item;
            float score;
        };
        std::vector<Pending> pending;

        /// The selectors offered to declare: Sema offers one for every
        /// method declaring it, its overrides among them.
        llvm::StringSet<> declared_selectors;

        /// `relevance` scales what the context makes of the candidate.
        auto add_declaration = [&](clang::CodeCompletionResult& candidate, float relevance) {
            // Sema offers a class's constructors with its name.
            const auto* tag = llvm::dyn_cast<clang::TagDecl>(candidate.Declaration);
            if(auto* constructor = llvm::dyn_cast_or_null<clang::CXXConstructorDecl>(
                   candidate.Declaration->getAsFunction())) {
                tag = constructor->getParent();
            }
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
            if((member_access || expected_type != 0) && expected(declaration)) {
                priority /= clang::CCF_ExactTypeMatch;
            }
            item.relevance = relevance * priority_weight(priority);
            if(unqualified && unit.in_library(declaration->getLocation())) {
                item.relevance *= 0.5F;
            }
            if(member_access && static_member(declaration)) {
                item.relevance *= 0.5F;
            }
            for(const auto& fix: candidate.FixIts) {
                if(auto removed = unit.written_range(fix.RemoveRange);
                   removed && focus.contains(removed->file)) {
                    item.edits.push_back({.range = removed->range, .text = fix.CodeToInsert});
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
            if(options.bundle_overloads && is_callable && !method) {
                item.bundle = item.label;
            }

            if(options.limit == 0) {
                render_declaration(candidate, std::move(item));
                return;
            }
            auto score = *match(item.filter.empty() ? item.label : item.filter) * item.relevance;
            if(item.deprecated) {
                score *= 0.5F;
            }
            pending.push_back({candidate, std::move(item), score});
        };

        for(auto& candidate: llvm::MutableArrayRef(candidates, candidate_count)) {
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
                    // A pattern carrying a C++ method is an override
                    // declaration in a class body, which the overrides below
                    // offer for every base, or in a method body a call of the
                    // overridden method — a duplicate of the method's own
                    // candidate that would change which function is called.
                    if(candidate.Declaration) {
                        if(const auto* method =
                               llvm::dyn_cast<clang::ObjCMethodDecl>(candidate.Declaration)) {
                            add_method_pattern(pattern, *method, candidate.Priority);
                        }
                        break;
                    }
                    // After `@selector(` Sema offers every selector it knows,
                    // from the piece the cursor is at.
                    if(context.getKind() == clang::CodeCompletionContext::CCC_SelectorName) {
                        auto typed = pattern.getAllTypedText();
                        add({
                            .label = typed,
                            .kind = protocol::CompletionItemKind::Method,
                            .insert = colon_follows ? llvm::StringRef(typed).split(':').first.str()
                                                    : std::string(),
                            .relevance = priority_weight(candidate.Priority),
                        });
                        break;
                    }
                    if(requirements) {
                        add_requirement(pattern, candidate.Priority);
                        break;
                    }
                    // After `#ifdef` a macro comes as a pattern of its name, an
                    // include guard among them, which Sema's other lists of
                    // macros leave out.
                    if(context.getKind() == clang::CodeCompletionContext::CCC_MacroNameUse) {
                        auto& pp = sema.getPreprocessor();
                        auto* info = pp.getMacroInfo(pp.getIdentifierInfo(pattern.getTypedText()));
                        if(info && info->isUsedForHeaderGuard()) {
                            break;
                        }
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
                    if(unqualified && info && unit.in_library(info->getDefinitionLoc())) {
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
        // file needs to reach them; after a qualifier naming a namespace,
        // those of it the file does not reach yet, the name alone.
        auto qualifying = unqualified
                              ? std::nullopt
                              : qualifying_namespace(sema, context, content, prefix.typed.begin);
        if(options.all_scopes && !prefix.spelling.empty() && !resolving &&
           (unqualified || qualifying)) {
            more_names = more_names || names.more_candidates;
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
            if(unqualified) {
                scan(ast.getTranslationUnitDecl());
            }

            auto import_at = action::import_insertion(content, sema.getLangOpts());
            llvm::StringRef own = module ? module->getTopLevelModuleName() : "";
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
            // What the name names at the point, which is not the candidate
            // the file does not reach.
            auto named_here = [&](llvm::StringRef name) {
                clang::LookupResult found(sema,
                                          &ast.Idents.get(name),
                                          sema.getPreprocessor().getCodeCompletionLoc(),
                                          clang::Sema::LookupOrdinaryName);
                found.suppressDiagnostics();
                return sema.LookupName(found, sema.getCurScope()) && !found.empty();
            };
            auto& language = sema.getLangOpts();
            bool uses_modules =
                language.CPlusPlusModules &&
                (named_module || ranges::any_of(ast.getTranslationUnitDecl()->noload_decls(),
                                                [](const clang::Decl* decl) {
                                                    return llvm::isa<clang::ImportDecl>(decl) &&
                                                           !decl->isImplicit();
                                                }));
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
                if(qualifying && (candidate.macro || !ranges::equal(segments, *qualifying))) {
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
                    // After a qualifier, Sema listed what the file reaches.
                    if(qualifying && !found.empty()) {
                        continue;
                    }
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
                // A file using modules takes the standard library from `std`;
                // a global module fragment imports nothing.
                bool imported = !candidate.module.empty() || (candidate.std_module && uses_modules);
                if((imported ? !options.insert_imports : !options.insert_includes) ||
                   (imported && !named_module && !imports)) {
                    continue;
                }
                std::optional<std::string> import;
                if(imported) {
                    import =
                        import_for(candidate.module.empty() ? llvm::StringRef("std")
                                                            : llvm::StringRef(candidate.module),
                                   own);
                    if(!import) {
                        continue;
                    }
                }
                // The written qualifier names its scope already.
                std::string qualifier;
                if(!qualifying) {
                    std::size_t shared = 0;
                    while(shared < segments.size() && shared < around.size() &&
                          segments[shared] == around[shared]->getName()) {
                        shared += 1;
                    }
                    if(shared < segments.size() ? shadowed(segments[shared], shared)
                                                : !candidate.macro && named_here(candidate.name)) {
                        // C names it by no other spelling.
                        if(!language.CPlusPlus) {
                            continue;
                        }
                        qualifier = "::";
                        shared = 0;
                    }
                    for(const auto& segment: views::drop(segments, shared)) {
                        qualifier += segment + "::";
                    }
                }
                Candidate item{
                    .label = qualifier + candidate.name,
                    .kind = candidate.kind,
                    .filter = candidate.name,
                    .deprecated = candidate.deprecated,
                    .relevance = 0.25F * priority_weight(clang::CCP_Declaration),
                };
                bool function = candidate.kind == protocol::CompletionItemKind::Function;
                // LIMITATION: an overload only the index knows, of a name the
                // file already sees, joins that name's bundle: accepting the
                // bundle adds no #include for it.
                if(options.bundle_overloads && function) {
                    item.bundle = item.label;
                }
                // Not knowing the parameters, a call takes its parentheses.
                if(options.arguments != CompletionArguments::None &&
                   !arguments_follow(candidate.is_template)) {
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
                        .text = std::move(*import)
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

        // The declarations the list keeps: those of the best `limit` labels,
        // a bundle of overloads one, as completion_list ranks them.
        {
            // In bundle mode one item of a label is shown (dedup_priority),
            // the first added among equals: a declaration an item added
            // already displaces is no candidate.
            llvm::StringMap<int> displacing;
            if(options.bundle_overloads) {
                for(auto& entry: collected) {
                    if(entry.item.kind != protocol::CompletionItemKind::Snippet) {
                        auto [it, inserted] =
                            displacing.try_emplace(entry.item.label, entry.dedup_priority);
                        it->second = std::max(it->second, entry.dedup_priority);
                    }
                }
            }
            auto displaced = [&](const Pending& each) {
                auto it = displacing.find(each.item.label);
                return it != displacing.end() &&
                       it->second >= dedup_priority(each.item.kind, each.item.macro);
            };
            llvm::StringMap<std::pair<float, std::optional<std::uint32_t>>> best;
            for(auto& each: pending) {
                if(displaced(each)) {
                    continue;
                }
                auto [it, inserted] =
                    best.try_emplace(each.item.label, each.score, each.item.order);
                if(!inserted && each.score > it->second.first) {
                    it->second = {each.score, std::nullopt};
                }
            }
            std::vector<std::pair<std::string, llvm::StringRef>> ranked;
            ranked.reserve(best.size());
            for(auto& entry: best) {
                ranked.emplace_back(sort_text(entry.second.first, entry.second.second),
                                    entry.getKey());
            }
            llvm::StringSet<> kept;
            auto keep = std::min<std::size_t>(ranked.size(), options.limit);
            more_names = more_names || keep < ranked.size();
            ranges::partial_sort(ranked, ranked.begin() + keep);
            for(auto& [order, label]: views::take(ranked, keep)) {
                kept.insert(label);
            }
            for(auto& each: pending) {
                if(kept.contains(each.item.label)) {
                    render_declaration(each.candidate, std::move(each.item));
                }
            }
        }

        for(auto& entry: overloads) {
            if(entry.count > 1) {
                entry.item.label_details = bundle_details(entry.count);
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

    /// The text completed in: the focus file's, but in a nested run in the
    /// whole buffer's compile, whose own reads a name at the point.
    llvm::StringRef text;

    std::uint32_t offset;

    PositionEncoding encoding;

    std::vector<protocol::CompletionItem>& output;

    /// What documents each item of `output`, in its order; read once the
    /// compile is done.
    std::vector<const clang::NamedDecl*>& documented;

    const CodeCompletionOptions& options;

    const CompletionClient& client;

    const CompileCompletion& names;

    /// The compile of the whole buffer, which tells what the code after the
    /// point does; none for a nested run in it.
    WholeBuffer* whole_buffer;

    bool typed_names_open = false;

    llvm::IntrusiveRefCntPtr<TypedNames> typed_names;

    /// In a nested run, the type the completion's own context expects, by
    /// its hash (EntityTable::type_hash): the whole buffer's compile has no
    /// parse at the point to tell it.
    std::uint64_t expected_type = 0;

    /// Set once an item needs the header of its symbol included (Completion).
    bool& includes_pending;

    /// Set when the reply holds fewer names than there are: the list's
    /// limit cut it, or the index or the loaded files have more of a scope
    /// for the name typed.
    bool& more_names;

    clang::CodeCompletionTUInfo info;

    /// The consumer Sema completes through in a tentative parse
    /// (getAllocator): Builder's results, none of the loaded files' names
    /// read.
    class Unloaded final : public clang::CodeCompleteConsumer {
    public:
        explicit Unloaded(Builder& builder) :
            clang::CodeCompleteConsumer(without_external(builder.CodeCompleteOpts)),
            builder(builder) {}

        /// A completion starting: one Sema left without a reply did not
        /// swap the builder back.
        clang::CodeCompletionAllocator& getAllocator() final {
            builder.unit.sema().CodeCompletion().CodeCompleter = &builder;
            return builder.getAllocator();
        }

        clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
            return builder.info;
        }

        void ProcessCodeCompleteResults(clang::Sema& sema,
                                        clang::CodeCompletionContext context,
                                        clang::CodeCompletionResult* candidates,
                                        unsigned candidate_count) final {
            sema.CodeCompletion().CodeCompleter = &builder;
            builder.unloaded_run = true;
            builder.ProcessCodeCompleteResults(sema, context, candidates, candidate_count);
        }

    private:
        static clang::CodeCompleteOptions without_external(clang::CodeCompleteOptions options) {
            options.LoadExternal = false;
            return options;
        }

    private:
        Builder& builder;
    };

    Unloaded unloaded{*this};

    /// The completion being processed ran through `unloaded`.
    bool unloaded_run = false;

    /// A reply is being processed: the completions it runs itself go
    /// through no `unloaded`.
    bool processing = false;

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
                   const CompileCompletion& names) -> Completion {
    auto offset = params.completion.offset;
    WholeBuffer whole_buffer(params);

    std::vector<protocol::CompletionItem> items;
    std::vector<const clang::NamedDecl*> documented;
    bool includes = false;
    bool more_names = false;
    params.completion.consumer = [&](CompilationUnit& unit) {
        return std::make_unique<Builder>(unit,
                                         offset,
                                         encoding,
                                         items,
                                         documented,
                                         options,
                                         client,
                                         names,
                                         &whole_buffer,
                                         includes,
                                         more_names);
    };
    auto unit = compile(std::move(params));

    assert(ranges::size(items) == ranges::size(documented));
    // An item's documentation costs most: those the list leaves out get
    // none. Where an item waits for its #include, the server cuts the list.
    auto limit = includes ? 0U : options.limit;
    bool cut = limit != 0 && items.size() > limit;
    if(cut) {
        std::vector<std::size_t> order(items.size());
        std::iota(order.begin(), order.end(), 0);
        auto key = [&](std::size_t index) {
            auto& item = items[index];
            return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text),
                            item.label);
        };
        ranges::partial_sort(order, order.begin() + limit, {}, key);
        std::vector<protocol::CompletionItem> kept;
        std::vector<const clang::NamedDecl*> kept_documented;
        for(auto index: views::take(order, limit)) {
            kept.push_back(std::move(items[index]));
            kept_documented.push_back(documented[index]);
        }
        items = std::move(kept);
        documented = std::move(kept_documented);
    }
    // LIMITATION: every listed item's documentation is read on each request,
    // its header lexed again for the comment: in a large file that is some
    // 20 ms of a completion, 40 to 70 with nothing typed, and part of why it
    // trails clangd's. A lazy completionItem/resolve would spare it.
    for(auto [item, decl]: views::zip(items, documented)) {
        if(!decl) {
            continue;
        }
        // A reply of the whole buffer's compile is of its declarations.
        auto* owner = whole_buffer.unit();
        if(!owner || &owner->context() != &decl->getASTContext()) {
            owner = unit.get();
        }
        auto doc = feature::documentation(*owner, decl, documentation);
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
        .list = {.is_incomplete = cut || more_names, .items = std::move(items)},
    };
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

}  // namespace clice::feature
