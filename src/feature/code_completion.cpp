module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :semantic.display;
import :semantic.resolver;
import :support.fuzzy_matcher;

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

/// In bundle mode several candidates may share one label: a class and its
/// constructors/deduction guides, or a declaration shadowed by a macro.
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

/// Extract the function signature (parameter list) from a CodeCompletionString.
/// Returns something like "(int x, float y)" for display in labelDetails.detail.
auto extract_signature(const clang::CodeCompletionString& ccs) -> std::string {
    std::string signature;
    bool in_parens = false;

    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        switch(chunk.Kind) {
            case CK::CK_LeftParen:
                in_parens = true;
                signature += '(';
                break;
            case CK::CK_RightParen:
                signature += ')';
                in_parens = false;
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
            case CK::CK_Informative: {
                // A function that cannot be called where completion happens
                // (a destructor after `T::`) carries its whole parameter list
                // as informative chunks, parentheses included.
                llvm::StringRef text = chunk.Text ? chunk.Text : "";
                if(text == "(") {
                    in_parens = true;
                }
                if(in_parens) {
                    signature += text;
                }
                if(text == ")") {
                    in_parens = false;
                }
                break;
            }
            case CK::CK_LeftAngle:
                signature += '<';
                in_parens = true;
                break;
            case CK::CK_RightAngle:
                signature += '>';
                in_parens = false;
                break;
            case CK::CK_Comma:
                if(in_parens) {
                    signature += ", ";
                }
                break;
            default: break;
        }
    }

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
    /// Sema surfaces macro candidates and statement keywords only on
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
    bool complete_resolved(clang::Sema& sema,
                           clang::CodeCompletionContext& context,
                           llvm::ArrayRef<clang::CodeCompletionResult> candidates) {
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
            /// For an arrow Sema reports the pointee: it only gets here once
            /// it has unwrapped the pointer itself.
            auto base = context.getBaseType();
            if(base.isNull() || !base->isDependentType()) {
                return false;
            }
            auto* tag = resolver.resolve_tag(base);
            if(!tag || same(tag, clang::HeuristicResolver(ast).resolveTypeToTagDecl(base))) {
                return false;
            }
            auto* record = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(definition(tag));
            if(!record) {
                return false;
            }
            auto type = ast.getQualifiedType(ast.getCanonicalTagType(record), base.getQualifiers());

            /// A designated initializer (`{ .§ }`) reports the same context
            /// as a member access but offers fields only, while a member
            /// access on a dependent base always offers more (the class
            /// name, the `template` keyword).
            bool designator =
                !candidates.empty() && std::ranges::all_of(candidates, [](const auto& candidate) {
                    return candidate.Kind == clang::CodeCompletionResult::RK_Declaration &&
                           llvm::isa<clang::FieldDecl>(candidate.Declaration);
                });
            if(designator) {
                /// Sema reports nothing at all for a class without fields;
                /// the resolved reply is still empty, not Sema's.
                if(record->fields().empty()) {
                    output.clear();
                    return true;
                }
                return run([&] {
                    sema.CodeCompletion().CodeCompleteDesignator(type, {}, clang::Designation());
                });
            }

            auto* object = new (ast) clang::OpaqueValueExpr(loc, type, clang::VK_LValue);
            return run([&] {
                sema.CodeCompletion().CodeCompleteMemberReferenceExpr(scope,
                                                                      object,
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
        } else if(complete_resolved(sema, context, {candidates, candidate_count})) {
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
        PositionMap map{.content = content, .lines = lines, .encoding = encoding};
        auto typed = map.to_range(prefix.typed);
        auto whole = map.to_range(prefix.whole);
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
            item.sort_text = std::format("{:.4f}", 2.0F - score);
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

        for(auto& candidate: llvm::make_range(candidates, candidates + candidate_count)) {
            switch(candidate.Kind) {
                case clang::CodeCompletionResult::RK_Keyword:
                    add({.label = candidate.Keyword,
                         .kind = protocol::CompletionItemKind::Keyword});
                    break;

                case clang::CodeCompletionResult::RK_Pattern: {
                    auto& pattern = *candidate.Pattern;
                    auto label = pattern.getAllTypedText();
                    auto head = pattern_text(pattern, /*whole=*/false);
                    // A pattern carrying a method is an override declaration
                    // in a class body, or in a method body a call of the
                    // overridden method — a duplicate of the method's own
                    // candidate that would change which function is called.
                    if(candidate.Declaration) {
                        if(!llvm::isa<clang::CXXRecordDecl>(sema.CurContext)) {
                            break;
                        }
                        add({
                            .label = label,
                            .kind = protocol::CompletionItemKind::Method,
                            .insert = head + ";",
                        });
                        break;
                    }
                    if(keyword_snippets) {
                        auto snippet = build_snippet(pattern);
                        bool has_snippet = !snippet.empty();
                        add({
                            .label = label,
                            .kind = protocol::CompletionItemKind::Snippet,
                            .insert = has_snippet ? std::move(snippet)
                                                  : pattern_text(pattern, /*whole=*/true),
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
                        item.signature = extract_signature(*ccs);
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
                    auto* declaration = candidate.Declaration;
                    // A hidden declaration is not what its name means here.
                    if(!declaration || candidate.Hidden ||
                       candidate.Availability == CXAvailability_NotAccessible) {
                        break;
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

                    bool is_callable = kind == protocol::CompletionItemKind::Function ||
                                       kind == protocol::CompletionItemKind::Method ||
                                       kind == protocol::CompletionItemKind::Constructor;
                    if(options.bundle_overloads && is_callable) {
                        item.bundle = item.label;
                    }

                    auto* ccs =
                        candidate.CreateCodeCompletionString(sema,
                                                             context,
                                                             getAllocator(),
                                                             getCodeCompletionTUInfo(),
                                                             /*IncludeBriefComments=*/false);
                    if(ccs) {
                        item.signature = extract_signature(*ccs);
                        item.return_type = extract_return_type(*ccs);
                        bool arguments = is_callable && !options.bundle_overloads &&
                                         options.enable_function_arguments_snippet;
                        bool template_arguments = options.enable_template_arguments_snippet &&
                                                  llvm::isa<clang::ClassTemplateDecl,
                                                            clang::TypeAliasTemplateDecl,
                                                            clang::VarTemplateDecl>(declaration);
                        if(client.snippets && !arguments_follow &&
                           (arguments || template_arguments)) {
                            item.insert = build_snippet(*ccs);
                            item.snippet = !item.insert.empty();
                            // Every parameter defaulted: nothing to fill in,
                            // but the name alone does not name a type.
                            if(template_arguments && !item.snippet) {
                                item.insert = item.label + "<>";
                            }
                        }
                    }

                    // A call gets its parentheses unless it has them already
                    // or the name is not being called here.
                    bool callable_here = candidate.FunctionCanBeCall &&
                                         !candidate.DeclaringEntity &&
                                         !context.isUsingDeclaration();
                    if(options.insert_paren_in_function_call && !item.snippet && callable_here &&
                       !arguments_follow &&
                       (kind == protocol::CompletionItemKind::Function ||
                        kind == protocol::CompletionItemKind::Method)) {
                        if(client.snippets) {
                            item.insert = escape_snippet(item.label) + "($0)";
                            item.snippet = true;
                        } else {
                            item.insert = item.label + "()";
                        }
                    }

                    add(item);
                    break;
                }
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
                   PositionEncoding encoding) -> std::vector<protocol::CompletionItem> {
    std::vector<protocol::CompletionItem> items;

    auto& [file, offset] = params.completion;
    (void)file;

    auto* consumer = new CodeCompletionCollector(offset, encoding, items, options, client);
    auto unit = complete(params, consumer);
    (void)unit;

    return items;
}

}  // namespace clice::feature
