module clice;

import :feature.code_actions.action;
import :feature.code_completion.cursor;
import :feature.code_completion.reply;
import :feature.code_completion;

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

/// The namespace a qualifier names, outermost first, inline and anonymous
/// ones left out.
struct Qualifying {
    std::vector<std::string> segments;

    /// Spelled from a namespace around the point, whichever declares it:
    /// none does where the file can see it yet.
    bool relative = false;
};

/// The namespace the qualifier of the name being completed names: as Sema
/// resolves it, or as the text spells it while no declaration of it is
/// visible (`std::` before any standard header). None for an unqualified
/// name, and one a class qualifies.
std::optional<Qualifying> qualifying_namespace(clang::Sema& sema,
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
        return Qualifying{
            .segments = written->segments | views::transform(&llvm::StringRef::str) |
                        ranges::to<std::vector>(),
            .relative = !written->global,
        };
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
    return Qualifying{.segments = std::move(segments)};
}

/// The namespaces around the completion point, outermost first: a
/// qualifier leaves them out.
struct NamespacesAround {
    explicit NamespacesAround(clang::Sema& sema) : sema(sema) {
        for(const auto* scope = sema.CurContext; scope; scope = scope->getParent()) {
            auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
            if(space && !space->isAnonymousNamespace() && !space->isInline()) {
                spaces.push_back(space->getCanonicalDecl());
            }
        }
        ranges::reverse(spaces);
    }

    /// Whether `scope` is the namespace `qualifying` names, which a relative
    /// one does from any namespace around the point.
    bool qualified_by(const Qualifying& qualifying, llvm::ArrayRef<std::string> scope) const {
        llvm::ArrayRef<std::string> written = qualifying.segments;
        if(scope.size() < written.size() ||
           !ranges::equal(scope.take_back(written.size()), written)) {
            return false;
        }
        auto from = scope.drop_back(written.size());
        return from.empty() ||
               (qualifying.relative && from.size() <= spaces.size() &&
                ranges::equal(from,
                              views::take(spaces, from.size()),
                              [](llvm::StringRef name, const clang::NamespaceDecl* space) {
                                  return name == space->getName();
                              }));
    }

    /// Whether a namespace or a class between the completion point and the
    /// namespace `shared` deep around it declares a scope named `name`: a
    /// qualifier starting with `name` names that one, and must start from
    /// the global scope instead.
    bool shadowed(llvm::StringRef name, std::size_t shared) const {
        const clang::Decl* home = shared == 0 ? nullptr : spaces[shared - 1];
        for(const auto* scope = sema.CurContext; !scope->isTranslationUnit();
            scope = scope->getParent()) {
            auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
            if(space && space->getCanonicalDecl() == home) {
                return false;
            }
            if(!space && !llvm::isa<clang::CXXRecordDecl>(scope)) {
                continue;
            }
            for(auto* found: scope->lookup(&sema.getASTContext().Idents.get(name))) {
                if(llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl, clang::TypeDecl>(
                       found) &&
                   sema.isVisible(found)) {
                    return true;
                }
            }
        }
        return false;
    }

    /// The qualifier naming `decl` from the completion point; none for a
    /// member of a class, or for what needs none.
    std::optional<clang::NestedNameSpecifier> qualifier_of(const clang::NamedDecl* decl) const {
        llvm::SmallVector<const clang::NamespaceDecl*, 4> named;
        for(const auto* scope = decl->getDeclContext(); !scope->isTranslationUnit();
            scope = scope->getParent()) {
            if(auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
                if(!space->isInline() && !space->isAnonymousNamespace()) {
                    named.push_back(space);
                }
            } else if(!scope->isTransparentContext()) {
                return std::nullopt;
            }
        }
        ranges::reverse(named);
        std::size_t shared = 0;
        while(shared < named.size() && shared < spaces.size() &&
              named[shared]->getCanonicalDecl() == spaces[shared]) {
            shared += 1;
        }
        if(shared == named.size()) {
            return std::nullopt;
        }
        clang::NestedNameSpecifier qualifier = std::nullopt;
        if(shadowed(named[shared]->getName(), shared)) {
            qualifier = clang::NestedNameSpecifier::getGlobal();
            shared = 0;
        }
        for(auto* space: views::drop(named, shared)) {
            qualifier = clang::NestedNameSpecifier(sema.getASTContext(), space, qualifier);
        }
        return qualifier;
    }

    clang::Sema& sema;

    llvm::SmallVector<const clang::NamespaceDecl*, 4> spaces;
};

/// What a namespace scope offers: not a name a using-declaration brings in
/// from another one, nor a specialization of a template it offers already.
bool offerable(const clang::NamedDecl* decl) {
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
           !llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
               decl);
}

/// Where only a type may be written, what names one, or a scope.
bool names_type(const clang::NamedDecl* decl) {
    return llvm::isa<clang::TypeDecl,
                     clang::ClassTemplateDecl,
                     clang::TypeAliasTemplateDecl,
                     clang::ConceptDecl,
                     clang::NamespaceDecl,
                     clang::NamespaceAliasDecl>(decl);
}

/// The declarations of `name` in `scope` as qualified lookup finds them:
/// through the inline and the anonymous namespaces in the scope too.
auto visible(clang::Sema& sema, clang::DeclContext* scope, llvm::StringRef name)
    -> llvm::SmallVector<clang::NamedDecl*, 2> {
    clang::LookupResult found(sema,
                              &sema.getASTContext().Idents.get(name),
                              {},
                              clang::Sema::LookupOrdinaryName);
    found.suppressDiagnostics();
    sema.LookupQualifiedName(found, scope);
    return llvm::SmallVector<clang::NamedDecl*, 2>(found.begin(), found.end());
}

/// Whether `name` names something at the point, which is not the candidate
/// the file does not reach.
bool named_here(clang::Sema& sema, llvm::StringRef name) {
    clang::LookupResult found(sema,
                              &sema.getASTContext().Idents.get(name),
                              sema.getPreprocessor().getCodeCompletionLoc(),
                              clang::Sema::LookupOrdinaryName);
    found.suppressDiagnostics();
    return sema.LookupName(found, sema.getCurScope()) && !found.empty();
}

}  // namespace

void Reply::add_other_namespaces(bool nested) {
    auto& options = completing.options;
    auto& client = completing.client;
    auto qualifying =
        unqualified() ? std::nullopt
                      : qualifying_namespace(sema, context, completing.text, prefix.typed.begin);
    // A nested run replacing Sema's reply completes a class, in a context
    // that may look unqualified; the one reading a namespace again
    // (`requires { typename lib:: }`) has it qualified.
    if(!options.all_scopes || !(qualifying || (unqualified() && !nested))) {
        return;
    }
    // The index is asked once a letter is typed (`std::` alone would
    // match all of it): until then the client must ask again.
    if(prefix.spelling.empty()) {
        completing.more_names = true;
        return;
    }
    completing.more_names = completing.more_names || completing.names.more_candidates;
    auto& ast = sema.getASTContext();
    NamespacesAround around(sema);
    bool types_only = context.getKind() == clang::CodeCompletionContext::CCC_Type;

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
               (types_only && !names_type(named)) || offered.contains(named->getCanonicalDecl()) ||
               !sema.isVisible(named)) {
                continue;
            }
            if(auto qualifier = around.qualifier_of(named)) {
                clang::CodeCompletionResult result(named,
                                                   clang::CCP_Declaration,
                                                   *qualifier,
                                                   /*QualifierIsInformative=*/false);
                add_declaration(result, 0.5F);
            }
        }
    };
    if(unqualified()) {
        scan(ast.getTranslationUnitDecl());
    }

    auto import_at = action::import_insertion(completing.text, sema.getLangOpts());
    auto* module = sema.getCurrentModule();
    bool named_module = module && module->isNamedModule();
    llvm::StringRef own = module ? module->getTopLevelModuleName() : "";
    auto& language = sema.getLangOpts();
    bool uses_modules =
        language.CPlusPlusModules &&
        (named_module ||
         ranges::any_of(ast.getTranslationUnitDecl()->noload_decls(), [](const clang::Decl* decl) {
             return llvm::isa<clang::ImportDecl>(decl) && !decl->isImplicit();
         }));
    for(const auto& candidate: completing.names.candidates) {
        // C names no namespace, an enumerator without its
        // enumeration, and a struct, union or enumeration only after
        // its keyword; a file compiled without named modules imports
        // nothing.
        using Kind = protocol::CompletionItemKind;
        llvm::ArrayRef<std::string> segments = candidate.scope;
        if(!language.CPlusPlus && candidate.kind == Kind::EnumMember) {
            segments = segments.drop_back();
        }
        if((!language.CPlusPlus &&
            (!segments.empty() || candidate.kind == Kind::Class || candidate.kind == Kind::Enum)) ||
           (!language.CPlusPlusModules && !candidate.module.empty()) ||
           ranges::any_of(segments,
                          [](llvm::StringRef segment) { return segment.starts_with("_"); })) {
            continue;
        }
        if(qualifying && (candidate.macro || !around.qualified_by(*qualifying, segments))) {
            continue;
        }
        // Sema offers the macros defined here already, and no
        // include brings back one the file undefined; a macro is no
        // declaration to look up.
        if(candidate.macro &&
           sema.getPreprocessor().getIdentifierInfo(candidate.name)->hadMacroDefinition()) {
            continue;
        }
        clang::DeclContext* scope = candidate.macro ? nullptr : ast.getTranslationUnitDecl();
        for(const auto& segment: segments) {
            auto found = visible(sema, scope, segment);
            auto space =
                ranges::find_if(found, llvm::IsaPred<clang::NamespaceDecl, clang::EnumDecl>);
            scope = space == found.end() ? nullptr : llvm::cast<clang::DeclContext>(*space);
            if(!scope) {
                break;
            }
        }
        if(scope) {
            auto found = visible(sema, scope, candidate.name);
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
                if(auto qualifier = around.qualifier_of(decl)) {
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
            import = import_for(candidate.module.empty() ? llvm::StringRef("std")
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
            while(shared < segments.size() && shared < around.spaces.size() &&
                  segments[shared] == around.spaces[shared]->getName()) {
                shared += 1;
            }
            if(shared < segments.size() ? around.shadowed(segments[shared], shared)
                                        : !candidate.macro && named_here(sema, candidate.name)) {
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
            if(function && !expects_function()) {
                item.insert =
                    client.snippets ? escape_snippet(item.label) + "($0)" : item.label + "()";
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
        add_item(item);
    }
}

}  // namespace clice::feature
