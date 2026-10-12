module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :semantic.display;
import :semantic.resolver;
import :support.fuzzy_matcher;
import :syntax.completion;

namespace clice::feature {

namespace {

/// Bytes of multi-byte UTF-8 sequences count too: clang accepts extended
/// characters in identifiers.
bool is_identifier_char(char c) {
    return clang::isAsciiIdentifierContinue(c) || static_cast<unsigned char>(c) >= 0x80;
}

/// The identifier the completion point sits in.
struct CompletionPrefix {
    /// From its start to the cursor: what candidates are matched against.
    LocalSourceRange typed;

    /// Through its end: what a chosen candidate replaces.
    LocalSourceRange whole;

    llvm::StringRef spelling;

    static auto from(llvm::StringRef content, std::uint32_t offset) -> CompletionPrefix {
        assert(offset <= content.size());

        auto start = offset;
        while(start > 0 && is_identifier_char(content[start - 1])) {
            start -= 1;
        }

        auto end = offset;
        while(end < content.size() && is_identifier_char(content[end])) {
            end += 1;
        }

        return CompletionPrefix{
            .typed = LocalSourceRange(start, offset),
            .whole = LocalSourceRange(start, end),
            .spelling = content.substr(start, offset - start),
        };
    }
};

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
    return std::ranges::min_element(paths, {}, &clang::CXXBasePath::Access)->Access;
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
        if(befriended && llvm::any_of(scopes, [&](const clang::NamedDecl* scope) {
               return befriends(befriended, scope);
           })) {
            return nullptr;
        }
    }
    return naming;
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
/// labelDetails.detail; parameters with a default show it.
auto extract_signature(const clang::CodeCompletionString& ccs, bool qualifiers) -> std::string {
    std::string signature;
    bool in_parens = false;
    bool in_angles = false;
    bool closed = false;

    auto extract = [&](this auto& self, const clang::CodeCompletionString& chunks) -> void {
        for(const auto& chunk: chunks) {
            using CK = clang::CodeCompletionString::ChunkKind;
            switch(chunk.Kind) {
                case CK::CK_LeftParen:
                    in_parens = true;
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

    return signature;
}

/// Text a snippet takes literally: `$` and `\` would otherwise start or
/// escape a snippet construct, and inside a placeholder `}` would end it.
auto escape_snippet(llvm::StringRef text, bool placeholder = false) -> std::string {
    std::string escaped;
    for(char c: text) {
        if(c == '$' || c == '\\' || (placeholder && c == '}')) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
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

    std::string signature;
    std::string return_type;
    bool deprecated = false;
    bool macro = false;

    /// Breaks ties in score: a field's place in a designated initializer.
    std::optional<std::uint32_t> order;
};

struct OverloadItem {
    protocol::CompletionItem item;
    float score = 0.0F;
    std::uint32_t count = 0;
    int dedup_priority = 0;
};

struct CollectedItem {
    protocol::CompletionItem item;
    int dedup_priority = 0;
};

class CodeCompletionCollector final : public clang::CodeCompleteConsumer {
public:
    /// Sema reports macro candidates and statement keywords only on
    /// request: `if`, `for` and the like exist only as code patterns.
    static clang::CodeCompleteOptions sema_options() {
        clang::CodeCompleteOptions options;
        options.IncludeMacros = 1;
        options.IncludeCodePatterns = 1;
        return options;
    }

    CodeCompletionCollector(std::uint32_t offset,
                            PositionEncoding encoding,
                            std::vector<protocol::CompletionItem>& output,
                            const CodeCompletionOptions& options,
                            const CompletionClient& client) :
        clang::CodeCompleteConsumer(sema_options()), offset(offset), encoding(encoding),
        output(output), options(options), client(client),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
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
            previous.swap(output);
            resolving = true;
            resolved_candidates = false;
            complete();
            resolving = false;
            if(!resolved_candidates) {
                output.swap(previous);
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
            auto object = reported;
            if(base) {
                if(!base->isTypeDependent()) {
                    return false;
                }
                object = resolver.member_object(base, kind == Kind::CCC_ArrowMemberAccess);
                if(object.isNull()) {
                    return false;
                }
            } else if(!reported->isDependentType()) {
                return false;
            }
            auto* tag = resolver.resolve_tag(object);
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
                bool named = std::ranges::any_of(record->decls(), [](const clang::Decl* decl) {
                    return llvm::isa<clang::FieldDecl, clang::IndirectFieldDecl>(decl) &&
                           llvm::cast<clang::NamedDecl>(decl)->getDeclName();
                });
                if(!named) {
                    output.clear();
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
            auto* tag = resolver.resolve_tag(clang::QualType(NNS.getAsType(), 0));
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
        if(context.getKind() == clang::CodeCompletionContext::CCC_Recovery) {
            return;
        }

        if(resolving) {
            resolved_candidates |= candidate_count > 0;
        } else if(complete_resolved(sema, context)) {
            return;
        }

        if(candidate_count == 0) {
            return;
        }

        // Clang's copy of the buffer carries a NUL at the completion
        // point; edits and look-ahead read the text as the user has it.
        auto& source_manager = sema.getSourceManager();
        auto buffer = source_manager.getBufferData(source_manager.getMainFileID());
        auto point = source_manager.getFileOffset(sema.getPreprocessor().getCodeCompletionLoc());
        auto content = buffer.take_front(point).str() + buffer.drop_front(point + 1).str();
        auto prefix = CompletionPrefix::from(content, offset);
        FuzzyMatcher matcher(prefix.spelling);

        auto lines = lsp::line_starts(content);
        PositionMap map(content, lines);
        auto typed = map.range(prefix.typed, encoding);
        auto whole = map.range(prefix.whole, encoding);
        if(!typed || !whole) {
            return;
        }
        // Call or template arguments already written after the name.
        auto after = llvm::StringRef(content).drop_front(prefix.whole.end).ltrim();
        bool arguments_follow = after.starts_with("(") || after.starts_with("<");

        std::vector<CollectedItem> collected;
        collected.reserve(candidate_count);

        std::vector<OverloadItem> overloads;
        overloads.reserve(candidate_count);
        std::unordered_map<std::string, std::size_t> overload_index;

        bool prefix_starts_with_underscore = prefix.spelling.starts_with("_");

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
            // Clients sort ascending; scores lie in (0, 2].
            auto sort_text = std::format("{:.4f}", 2.0F - score);
            if(candidate.order) {
                sort_text += std::format("{:05}", *candidate.order);
            }
            item.sort_text = std::move(sort_text);
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
            llvm::StringRef filter = candidate.filter.empty() ? candidate.label : candidate.filter;
            if(filter.empty()) {
                return;
            }

            // Filter out _/__ prefixed internal symbols unless user typed _.
            if(!prefix_starts_with_underscore && filter.starts_with("_")) {
                return;
            }

            auto score = matcher.match(filter);
            if(!score.has_value()) {
                return;
            }

            int priority = dedup_priority(candidate.kind, candidate.macro);

            if(!candidate.bundle.empty()) {
                auto [it, inserted] =
                    overload_index.try_emplace(candidate.bundle, overloads.size());
                if(inserted) {
                    overloads.push_back({
                        .item = build_item(candidate, *score),
                        .score = *score,
                        .count = 1,
                        .dedup_priority = priority,
                    });
                } else {
                    auto& existing = overloads[it->second];
                    existing.count += 1;
                    if(*score > existing.score) {
                        existing.score = *score;
                        existing.item.sort_text = std::format("{:.4f}", 2.0F - *score);
                    }
                }
                return;
            }

            collected.push_back(
                {.item = build_item(candidate, *score), .dedup_priority = priority});
        };

        bool keyword_snippets = options.enable_keyword_snippet && client.snippets;
        llvm::StringSet<> plain_patterns;
        auto add_pattern = [&](const clang::CodeCompletionString& pattern) {
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
                });
            } else if(plain_patterns.insert(head).second) {
                // Without its placeholders a pattern is its keyword;
                // the variants of one statement collapse.
                add({
                    .label = label,
                    .kind = protocol::CompletionItemKind::Keyword,
                    .insert = head,
                });
            }
        };

        bool member_access =
            context.getKind() == clang::CodeCompletionContext::CCC_DotMemberAccess ||
            context.getKind() == clang::CodeCompletionContext::CCC_ArrowMemberAccess;
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
            for(auto designator: written_designators(content, prefix.typed.begin)) {
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

        /// What accepting a call inserts, unless its arguments are written
        /// already: the arguments to fill in, else, when the name is called
        /// here, its parentheses.
        auto insert_call =
            [&](Candidate& item, const clang::CodeCompletionString& ccs, bool called_here) {
                if(arguments_follow) {
                    return;
                }
                if(client.snippets && !options.bundle_overloads &&
                   options.enable_function_arguments_snippet) {
                    item.insert = build_snippet(ccs);
                    item.snippet = !item.insert.empty();
                }
                if(called_here && options.insert_paren_in_function_call && !item.snippet) {
                    if(client.snippets) {
                        item.insert = escape_snippet(item.label) + "($0)";
                        item.snippet = true;
                    } else {
                        item.insert = item.label + "()";
                    }
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
        auto add_requirement = [&](const clang::CodeCompletionString& pattern) {
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
            add(item);
        };

        auto add_declaration = [&](clang::CodeCompletionResult& candidate) {
            auto* declaration = candidate.Declaration;
            // A qualifier clang does not mark informative must be
            // written: the name alone does not reach the
            // declaration (a scoped enumerator outside its enum).
            std::string qualifier;
            if(candidate.Qualifier && !candidate.QualifierIsInformative) {
                llvm::raw_string_ostream stream(qualifier);
                candidate.Qualifier.print(stream, sema.getPrintingPolicy());
            }

            auto kind = completion_kind(declaration);
            auto name = display::name_of(declaration,
                                         {
                                             .suppress_ctor_template_args = true,
                                             .qualified = false,
                                         });
            Candidate item{
                .label = qualifier + name,
                .kind = kind,
                .filter = qualifier.empty() ? std::string() : name,
                .deprecated = candidate.Availability == CXAvailability_Deprecated,
            };

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

            auto& ccs = *candidate.CreateCodeCompletionString(sema,
                                                              context,
                                                              getAllocator(),
                                                              getCodeCompletionTUInfo(),
                                                              /*IncludeBriefComments=*/false);
            // Clang gives a variable holding a lambda the signature of its
            // call operator, whose `const` is no part of calling it.
            bool method = llvm::isa_and_present<clang::CXXMethodDecl>(declaration->getAsFunction());
            item.signature = extract_signature(ccs, /*qualifiers=*/method);
            item.return_type = extract_return_type(ccs);
            if(is_callable) {
                insert_call(item,
                            ccs,
                            kind != protocol::CompletionItemKind::Constructor &&
                                candidate.FunctionCanBeCall && !candidate.DeclaringEntity &&
                                !context.isUsingDeclaration());
            } else if(options.enable_template_arguments_snippet && client.snippets &&
                      !arguments_follow &&
                      llvm::isa<clang::ClassTemplateDecl,
                                clang::TypeAliasTemplateDecl,
                                clang::VarTemplateDecl>(declaration)) {
                item.insert = build_snippet(ccs);
                item.snippet = !item.insert.empty();
                // Every parameter defaulted: nothing to fill in,
                // but the name alone does not name a type.
                if(!item.snippet) {
                    item.insert = item.label + "<>";
                }
            }

            add(item);
        };

        for(auto& candidate: llvm::make_range(candidates, candidates + candidate_count)) {
            switch(candidate.Kind) {
                case clang::CodeCompletionResult::RK_Keyword:
                    add({.label = candidate.Keyword,
                         .kind = protocol::CompletionItemKind::Keyword});
                    break;

                case clang::CodeCompletionResult::RK_Pattern: {
                    auto& pattern = *candidate.Pattern;
                    // A pattern carrying a method is an override declaration
                    // in a class body, or in a method body a call of the
                    // overridden method — a duplicate of the method's own
                    // candidate that would change which function is called.
                    if(candidate.Declaration) {
                        if(!llvm::isa<clang::CXXRecordDecl>(sema.CurContext)) {
                            break;
                        }
                        add({
                            .label = pattern.getAllTypedText(),
                            .kind = protocol::CompletionItemKind::Method,
                            .insert = pattern_text(pattern, /*whole=*/false) + ";",
                        });
                        break;
                    }
                    if(requirements) {
                        add_requirement(pattern);
                        break;
                    }
                    add_pattern(pattern);
                    break;
                }

                case clang::CodeCompletionResult::RK_Macro: {
                    auto* info = sema.getPreprocessor().getMacroInfo(candidate.Macro);
                    bool function_like = info && info->isFunctionLike();

                    Candidate item{
                        .label = candidate.Macro->getName().str(),
                        .kind = function_like ? protocol::CompletionItemKind::Function
                                              : protocol::CompletionItemKind::Constant,
                        .macro = true,
                    };
                    if(auto* ccs =
                           candidate.CreateCodeCompletionString(sema,
                                                                context,
                                                                getAllocator(),
                                                                getCodeCompletionTUInfo(),
                                                                /*IncludeBriefComments=*/false)) {
                        item.signature = extract_signature(*ccs, /*qualifiers=*/false);
                        if(function_like && options.enable_function_arguments_snippet &&
                           client.snippets) {
                            item.insert = build_snippet(*ccs);
                            item.snippet = !item.insert.empty();
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
                        add_declaration(candidate);
                        break;
                    }
                    for(auto* target: targets) {
                        auto named = candidate;
                        named.Declaration = target->getUnderlyingDecl();
                        add_declaration(named);
                    }
                    break;
                }
            }
        }

        // SemaCodeComplete's `import name;` pattern skips implementation
        // units, which import past their module declaration just the same.
        if(context.getKind() == clang::CodeCompletionContext::CCC_TopLevel &&
           sema.CurContext->isTranslationUnit()) {
            if(auto* module = sema.getCurrentModule();
               module && (module->Kind == clang::Module::ModuleImplementationUnit ||
                          module->Kind == clang::Module::ModulePartitionImplementation)) {
                clang::CodeCompletionBuilder builder(getAllocator(), getCodeCompletionTUInfo());
                builder.AddTypedTextChunk("import");
                builder.AddChunk(clang::CodeCompletionString::CK_HorizontalSpace);
                builder.AddPlaceholderChunk("name");
                builder.AddChunk(clang::CodeCompletionString::CK_SemiColon);
                builder.AddChunk(clang::CodeCompletionString::CK_VerticalSpace);
                add_pattern(*builder.TakeString());
            }
        }

        for(auto& entry: overloads) {
            if(entry.count > 1) {
                protocol::CompletionItemLabelDetails details;
                details.detail = std::format("(…) +{} overloads", entry.count);
                entry.item.label_details = std::move(details);
            }
            collected.push_back(
                {.item = std::move(entry.item), .dedup_priority = entry.dedup_priority});
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
        output.reserve(collected.size());
        for(auto& entry: collected) {
            output.push_back(std::move(entry.item));
        }
    }

private:
    std::uint32_t offset;
    PositionEncoding encoding;
    std::vector<protocol::CompletionItem>& output;
    const CodeCompletionOptions& options;
    const CompletionClient& client;
    clang::CodeCompletionTUInfo info;
    bool resolving = false;
    bool resolved_candidates = false;
};

}  // namespace

auto code_complete(CompilationParams& params,
                   const CodeCompletionOptions& options,
                   const CompletionClient& client,
                   PositionEncoding encoding) -> protocol::CompletionList {
    auto& [file, offset] = params.completion;
    (void)file;

    std::vector<protocol::CompletionItem> items;
    auto* consumer = new CodeCompletionCollector(offset, encoding, items, options, client);
    auto unit = complete(params, consumer);
    (void)unit;

    return completion_list(std::move(items), options.limit);
}

auto completion_list(std::vector<protocol::CompletionItem> items, std::uint32_t limit)
    -> protocol::CompletionList {
    protocol::CompletionList list{.is_incomplete = false, .items = std::move(items)};
    if(limit != 0 && list.items.size() > limit) {
        std::ranges::sort(list.items, {}, [](const protocol::CompletionItem& item) {
            return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text),
                            item.label);
        });
        list.items.resize(limit);
        list.is_incomplete = true;
    }
    return list;
}

}  // namespace clice::feature
