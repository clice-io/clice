module clice;

import :compile.compilation;
import :feature.code_actions.action;
import :feature.code_completion.cursor;
import :feature.code_completion.reply;
import :feature.code_completion.typed_names;
import :feature.code_completion.whole_buffer;
import :feature.code_completion;
import :feature.documentation;
import :semantic.decls;
import :semantic.identity;
import :semantic.template_resolver;
import :support.json;

namespace clice::feature {

namespace {

/// Make `item`, one overload of a name, stand for all `count` of them: a
/// call it inserts takes its parentheses, with the cursor between them for a
/// client taking snippets, in place of its own arguments.
void bundle(protocol::CompletionItem& item, std::size_t count, bool snippets) {
    item.label_details = bundle_details(count);
    if(!snippets) {
        return;
    }
    std::visit(
        [&](auto& edit) {
            if(llvm::StringRef(edit.new_text).starts_with(item.label + "(")) {
                edit.new_text = escape_snippet(item.label) + "($0)";
                item.insert_text_format = protocol::InsertTextFormat::Snippet;
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

    Builder(Completing completing, std::vector<Item>& output, WholeBuffer* whole_buffer) :
        clang::CodeCompleteConsumer(sema_options(completing.options)),
        completing(std::move(completing)), output(output), whole_buffer(whole_buffer) {}

    clang::CodeCompletionAllocator& getAllocator() final {
        auto& sema = completing.unit.sema();
        // Sema asks first thing in a completion, before it reads any name.
        // A nested run completes in the whole buffer's compile, whose files
        // it reads whole.
        if(whole_buffer && !typed_names_open) {
            typed_names_open = true;
            if(auto typed = Prefix::from(completing.text, completing.offset).spelling;
               !typed.empty()) {
                typed_names = TypedNames::open(sema, typed);
            }
        }
        // A tentative parse reaching the point mostly runs a completion of
        // its recovery, whose names ProcessCodeCompleteResults drops: with
        // nothing typed, Sema would read every name the loaded files
        // declare for it. Sema asks the consumer whether to read them after
        // it asks for the allocator, so that completion runs through
        // `unloaded`, which reads none.
        auto& completion = sema.CodeCompletion();
        if(whole_buffer && !typed_names && nested_run == Nested::None &&
           completion.CodeCompleter == this && sema.getPreprocessor().isBacktrackEnabled()) {
            completion.CodeCompleter = &unloaded;
        }
        return completing.info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return completing.info;
    }

    void ProcessCodeCompleteResults(clang::Sema& sema,
                                    clang::CodeCompletionContext context,
                                    clang::CodeCompletionResult* candidates,
                                    unsigned candidate_count) final {
        process(sema,
                context,
                llvm::MutableArrayRef(candidates, candidate_count),
                /*through_unloaded=*/false);
    }

private:
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
            builder.completing.unit.sema().CodeCompletion().CodeCompleter = &builder;
            return builder.getAllocator();
        }

        clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
            return builder.completing.info;
        }

        void ProcessCodeCompleteResults(clang::Sema& sema,
                                        clang::CodeCompletionContext context,
                                        clang::CodeCompletionResult* candidates,
                                        unsigned candidate_count) final {
            sema.CodeCompletion().CodeCompleter = &builder;
            builder.process(sema,
                            context,
                            llvm::MutableArrayRef(candidates, candidate_count),
                            /*through_unloaded=*/true);
        }

    private:
        static clang::CodeCompleteOptions without_external(clang::CodeCompleteOptions options) {
            options.LoadExternal = false;
            return options;
        }

    private:
        Builder& builder;
    };

    /// Where a nested run (replace) stands that replaces Sema's reply: its
    /// own reply is the one processed, and one offering no candidates
    /// leaves Sema's.
    enum class Nested : std::uint8_t {
        None,
        Running,
        Offered,
    };

    /// One of Sema's replies, which came through `unloaded` when
    /// `through_unloaded`.
    void process(clang::Sema& sema,
                 clang::CodeCompletionContext& context,
                 llvm::MutableArrayRef<clang::CodeCompletionResult> candidates,
                 bool through_unloaded) {
        // In a conditional block the build turns off, clang lexes raw and
        // offers the names of whatever scope the parser stood in when its
        // lookahead reached the block: the scope a preamble's PCH leaves
        // differs from the one a whole parse does.
        auto* lexer = sema.getPreprocessor().getCurrentFileLexer();
        if(context.getKind() == clang::CodeCompletionContext::CCC_Recovery ||
           (lexer && lexer->isLexingRawMode())) {
            return;
        }
        if(through_unloaded && complete_namespace(sema, context)) {
            return;
        }

        if(nested_run != Nested::None) {
            if(!candidates.empty()) {
                nested_run = Nested::Offered;
            }
        } else {
            completing.display_options.specialization = nullptr;
            if(complete_resolved(sema, context)) {
                return;
            }
            completing.more_names = typed_names && typed_names->more();
        }

        if(candidates.empty() && completing.names.candidates.empty()) {
            return;
        }
        auto reply = Reply::open(completing, context);
        if(!reply) {
            return;
        }
        reply->add_candidates(candidates);
        reply->add_overrides();
        reply->add_other_namespaces(/*nested=*/nested_run != Nested::None);
        reply->add_import();
        output = reply->finish();
    }

    /// A tentative parse that keeps what it parsed at the point
    /// (`requires { typename lib:: }`) made the completion that stands
    /// through `unloaded`: the names of a namespace, or of the global one,
    /// qualifying it come again, read. Whether that produced the reply.
    bool complete_namespace(clang::Sema& sema, clang::CodeCompletionContext& context) {
        using enum clang::NestedNameSpecifier::Kind;
        auto spec = context.getCXXScopeSpecifier();
        if(!spec || ((*spec)->getScopeRep().getKind() != Namespace &&
                     (*spec)->getScopeRep().getKind() != Global)) {
            return false;
        }
        auto qualifier = **spec;
        return replace([&] {
            sema.CodeCompletion().CodeCompleteQualifiedId(sema.getCurScope(),
                                                          qualifier,
                                                          /*EnteringContext=*/false,
                                                          context.isUsingDeclaration(),
                                                          /*IsAddressOfOperand=*/false,
                                                          /*IsInDeclarationContext=*/false,
                                                          /*BaseType=*/{},
                                                          context.getPreferredType());
        });
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
        std::vector<Item> previous;
        previous.swap(output);
        nested_run = Nested::Running;
        complete();
        if(std::exchange(nested_run, Nested::None) != Nested::Offered) {
            output.swap(previous);
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
        auto found = whole_buffer->instantiated(templated, loop, completing.unit);
        if(!found) {
            return false;
        }
        auto* pattern = enclosing_template(found->context);
        auto calls = pattern ? decls::instantiations(*pattern->getDescribedFunctionTemplate())
                             : llvm::SmallVector<clang::FunctionDecl*, 4>();
        auto& options = completing.options;
        // LIMITATION: a generic lambda inside a function template is never
        // instantiated itself, only the copies in the template's
        // instantiations are: in its body, a name of the template's
        // parameters completes through the template's only instantiation, and
        // to nothing when the template has several.
        if(calls.size() < 2) {
            return complete_through(*found, nullptr, preferred, options, output);
        }
        if(found->constexpr_branch) {
            llvm::erase_if(calls,
                           [&](clang::FunctionDecl* call) { return found->discarded_by(*call); });
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
        every.limit = NoLimit::Unlimited;
        every.bundle_overloads = false;
        auto name_of = [](const protocol::CompletionItem& item) -> llvm::StringRef {
            return item.filter_text ? *item.filter_text : item.label;
        };
        std::vector<Item> items;
        std::set<std::pair<std::string, std::string>> listed;
        llvm::StringMap<std::size_t> having;
        for(auto* call: calls) {
            std::vector<Item> offered;
            if(!complete_through(*found, call, preferred, every, offered)) {
                return false;
            }
            llvm::StringSet<> names;
            for(auto& entry: offered) {
                names.insert(name_of(entry.item));
                auto& details = entry.item.label_details;
                auto overload = details && details->detail ? *details->detail : std::string();
                if(listed.emplace(entry.item.label, std::move(overload)).second) {
                    items.push_back(std::move(entry));
                }
            }
            for(auto& name: names) {
                having[name.getKey()] += 1;
            }
        }
        output.clear();
        // The overloads in each bundle, by the item standing for it.
        llvm::StringMap<std::size_t> bundles;
        std::vector<std::size_t> overloads;
        for(auto& entry: items) {
            auto& item = entry.item;
            if(having.lookup(name_of(item)) != calls.size()) {
                continue;
            }
            using enum protocol::CompletionItemKind;
            if(options.bundle_overloads &&
               (item.kind == Function || item.kind == Method || item.kind == Constructor)) {
                auto [it, inserted] = bundles.try_emplace(item.label, output.size());
                if(!inserted) {
                    overloads[it->second] += 1;
                    output[it->second].item.sort_text =
                        std::min(*output[it->second].item.sort_text, *item.sort_text);
                    continue;
                }
            }
            output.push_back(std::move(entry));
            overloads.push_back(1);
        }
        for(auto&& [entry, count]: views::zip(output, overloads)) {
            if(count > 1) {
                bundle(entry.item, count, completing.client.snippets);
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
                          std::vector<Item>& items) {
        auto& other = found.unit.sema();
        auto& ast = found.unit.context();

        const static CompileCompletion none;
        bool includes = false;
        bool more = false;
        std::uint64_t expected_type = 0;
        if(!preferred.isNull() && !preferred->isDependentType()) {
            expected_type = completing.unit.get<EntityTable>().type_hash(
                preferred.getNonReferenceType().getUnqualifiedType());
        }
        Builder nested(
            {
                .unit = found.unit,
                .text = completing.text,
                .offset = completing.offset,
                .encoding = completing.encoding,
                .options = run_options,
                .client = completing.client,
                .names = none,
                .expected_type = expected_type,
                .includes_pending = includes,
                .more_names = more,
            },
            items,
            nullptr);
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
            nested.completing.display_options.specialization =
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
        completing.more_names = completing.more_names || (answered && more);
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
        auto& display_options = completing.display_options;
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

private:
    Completing completing;

    std::vector<Item>& output;

    /// The compile of the whole buffer, which tells what the code after the
    /// point does; none for a nested run in it.
    WholeBuffer* whole_buffer;

    bool typed_names_open = false;

    llvm::IntrusiveRefCntPtr<TypedNames> typed_names;

    Unloaded unloaded{*this};

    /// The completions a nested run makes go through no `unloaded`.
    Nested nested_run = Nested::None;
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

    std::vector<Item> items;
    bool includes = false;
    bool more_names = false;
    params.completion.consumer = [&](CompilationUnit& unit) {
        return std::make_unique<Builder>(
            Completing{
                .unit = unit,
                .text = unit.focus_file().content(),
                .offset = offset,
                .encoding = encoding,
                .options = options,
                .client = client,
                .names = names,
                .includes_pending = includes,
                .more_names = more_names,
            },
            items,
            &whole_buffer);
    };
    auto unit = compile(std::move(params));

    // An item's documentation costs most: those the list leaves out get
    // none. Where an item waits for its #include, the server cuts the list.
    auto limit = includes ? at_most(NoLimit::Unlimited) : at_most(options.limit);
    bool cut = items.size() > limit;
    if(cut) {
        std::vector<std::size_t> order(items.size());
        std::iota(order.begin(), order.end(), 0);
        auto key = [&](std::size_t index) {
            auto& item = items[index].item;
            return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text),
                            item.label);
        };
        ranges::partial_sort(order, order.begin() + limit, {}, key);
        std::vector<Item> kept;
        for(auto index: views::take(order, limit)) {
            kept.push_back(std::move(items[index]));
        }
        items = std::move(kept);
    }
    // LIMITATION: every listed item's documentation is read on each request,
    // its header lexed again for the comment: in a large file that is some
    // 20 ms of a completion, 40 to 70 with nothing typed, and part of why it
    // trails clangd's. A lazy completionItem/resolve would spare it.
    for(auto& [item, decl]: items) {
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

    auto listed = items | views::transform([](Item& entry) { return std::move(entry.item); }) |
                  ranges::to<std::vector>();
    Completion completion{
        .list = {.is_incomplete = cut || more_names, .items = std::move(listed)},
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
