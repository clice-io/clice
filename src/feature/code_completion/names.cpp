module clice;

import :feature.code_completion.names;
import :feature.code_completion;
import :index.search;
import :support.fuzzy_matcher;
import :support.logging;
import :support.timer;

namespace clice::feature {

namespace {

/// The qualifier `scope` is as the parse reads it: a macro spelling one of
/// its scopes expands to the names and `::` it spells, through the macros it
/// expands in turn. As written where a macro spells other tokens.
index::CompletionScope expanded_qualifier(clang::Preprocessor& pp,
                                          const index::CompletionScope& scope) {
    if(scope.qualifier.empty()) {
        return scope;
    }
    index::CompletionScope expanded{.absolute = scope.absolute};
    auto expand = [&](this auto& self, llvm::StringRef name, std::uint32_t depth) -> bool {
        auto* macro = pp.getMacroInfo(pp.getIdentifierInfo(name));
        if(!macro) {
            expanded.qualifier.push_back(name.str());
            return true;
        }
        if(macro->isFunctionLike() || depth == 8) {
            return false;
        }
        for(const auto& token: macro->tokens()) {
            if(token.is(clang::tok::coloncolon)) {
                expanded.absolute |= expanded.qualifier.empty();
            } else if(auto* spelled = token.getIdentifierInfo();
                      !spelled || !self(spelled->getName(), depth + 1)) {
                return false;
            }
        }
        return true;
    };
    for(const auto& name: scope.qualifier) {
        if(!expand(name, 0)) {
            return scope;
        }
    }
    // A first scope a macro expands to nothing leaves its `::` naming the
    // global one.
    expanded.absolute |= expanded.qualifier.empty();
    return expanded;
}

/// The namespace a qualifier names at the point, through aliases and
/// macros; null for another scope, whose names Sema reads whole regardless.
clang::DeclContext* qualified_namespace(clang::Sema& sema, const index::CompletionScope& written) {
    auto scope = expanded_qualifier(sema.getPreprocessor(), written);
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

/// The identifiers of the files the compile loads that `typed` matches,
/// best first. Read from the files' identifier tables, which name every
/// identifier they spell and load no declaration.
std::vector<llvm::StringRef> matching_identifiers(clang::ASTContext& ast, llvm::StringRef typed) {
    auto* lookup = ast.Idents.getExternalIdentifierLookup();
    if(!lookup) {
        return {};
    }
    auto lower = typed.lower();
    // A name reserved to the implementation is the library's own, wanted
    // only when typed so.
    bool reserved_typed = typed.starts_with("_");
    FuzzyMatcher matcher(typed);
    std::vector<std::pair<float, llvm::StringRef>> matched;
    std::unique_ptr<clang::IdentifierIterator> identifiers(lookup->getIdentifiers());
    for(auto name = identifiers->Next(); !name.empty(); name = identifiers->Next()) {
        if(!reserved_typed && name.starts_with("_") &&
           (name.starts_with("__") || (name.size() > 1 && llvm::isUpper(name[1])))) {
            continue;
        }
        // The matcher's cost would add up over every identifier of a large
        // preamble: a name is none of its matches when the typed characters
        // are not in it in order, the first on what may begin a word — its
        // start, past a separator, an uppercase letter.
        std::size_t next = 0;
        for(std::size_t i = 0; i < name.size() && next < lower.size(); i += 1) {
            char c = name[i];
            if(llvm::toLower(c) != lower[next]) {
                continue;
            }
            if(next == 0 && i != 0 && llvm::isAlnum(lower[0]) && llvm::isAlnum(name[i - 1]) &&
               !llvm::isUpper(c)) {
                continue;
            }
            next += 1;
        }
        if(next != lower.size()) {
            continue;
        }
        if(auto score = matcher.match(name)) {
            matched.emplace_back(*score, name);
        }
    }
    ranges::sort(matched, [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    std::vector<llvm::StringRef> names;
    names.reserve(matched.size());
    for(auto& [score, name]: matched) {
        names.push_back(name);
    }
    return names;
}

}  // namespace

LoadedNames load_names(clang::Sema& sema, const CompileCompletion& names) {
    auto& ast = sema.getASTContext();
    auto& scope = names.scope;
    LoadedNames loaded;
    auto& space = loaded.space;
    if(scope.qualified()) {
        space = qualified_namespace(sema, scope);
        if(!space) {
            return loaded;
        }
        llvm::SmallVector<llvm::StringRef, 4> spelled;
        for(auto* context = space; !context->isTranslationUnit(); context = context->getParent()) {
            if(auto* named = llvm::dyn_cast<clang::NamespaceDecl>(context);
               named && !named->isInline()) {
                spelled.push_back(named->getName());
            }
        }
        ranges::reverse(spelled);
        if(spelled.size() < scope.qualifier.size() ||
           !ranges::equal(llvm::ArrayRef(spelled).take_back(scope.qualifier.size()),
                          scope.qualifier,
                          {},
                          {},
                          [](const std::string& name) { return llvm::StringRef(name); })) {
            read_names(space);
        }
    }
    // An identifier comes with the macro the loaded files define by its
    // name. Whether it names something where it is looked up; a keyword
    // names nothing.
    auto load = [&](llvm::StringRef name, bool macro) {
        auto* identifier = &ast.Idents.get(name);
        if(identifier->isKeyword(sema.getLangOpts())) {
            return false;
        }
        clang::LookupResult found(sema, identifier, {}, clang::Sema::LookupOrdinaryName);
        found.suppressDiagnostics();
        if(space) {
            sema.LookupQualifiedName(found, space);
        } else if(!macro) {
            sema.LookupName(found, sema.getCurScope());
        }
        return !found.empty() || (!space && identifier->hasMacroDefinition());
    };
    for(const auto& candidate: names.candidates) {
        load(candidate.name, candidate.macro);
    }
    // After a member access, Sema reads the class whole.
    if(!names.prefix) {
        return loaded;
    }
    if(names.prefix->empty()) {
        // Every identifier matches: the index's most used names stand for a
        // scope it holds more of than a completion takes, and one it holds
        // no more of — small, or not indexed — costs little read whole,
        // with the macros where no qualifier was written.
        if(!names.more_candidates) {
            read_names(space ? space : ast.getTranslationUnitDecl());
            if(!space) {
                sema.getPreprocessor().macros(/*IncludeExternalMacros=*/true);
            }
        }
        return loaded;
    }
    // Most identifiers name nothing here: members, parameters, names of
    // other namespaces. They are looked up best first until as many name
    // something as a completion takes from the index, a bounded number of
    // them.
    ScopedTimer scan_timer;
    auto matching = matching_identifiers(ast, *names.prefix);
    auto scan_ms = scan_timer.ms_f();
    ScopedTimer lookup_timer;
    std::size_t found = 0;
    std::size_t tried = 0;
    for(auto name: matching) {
        if(found == scope_candidate_limit || tried == scope_candidate_limit * 40) {
            loaded.more = true;
            break;
        }
        tried += 1;
        found += load(name, false);
    }
    LOG_PERF("completion",
             "op=names matched={} tried={} found={} scan_ms={:.2f} lookup_ms={:.2f}",
             matching.size(),
             tried,
             found,
             scan_ms,
             lookup_timer.ms_f());
    return loaded;
}

bool brings_in(clang::ASTContext& ast, const clang::DeclContext* space) {
    using Kind = clang::Decl::Kind;
    auto wanted = [](Kind kind) {
        return kind == Kind::Using || kind == Kind::UsingEnum || kind == Kind::UsingPack ||
               kind == Kind::UsingDirective || kind == Kind::Namespace || kind == Kind::LinkageSpec;
    };
    auto* source = ast.getExternalSource();
    llvm::SmallVector<const clang::DeclContext*, 8> pending;
    auto enter = [&](const clang::DeclContext* context) {
        if(auto* named = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            for(auto* each: named->redecls()) {
                pending.push_back(each);
            }
        } else {
            pending.push_back(context);
        }
    };
    enter(space);
    while(!pending.empty()) {
        const auto* context = pending.pop_back_val();
        llvm::SmallVector<clang::Decl*, 16> decls;
        for(auto* decl: context->noload_decls()) {
            if(wanted(decl->getKind())) {
                decls.push_back(decl);
            }
        }
        if(source && context->hasExternalLexicalStorage()) {
            source->FindExternalLexicalDecls(context, wanted, decls);
        }
        for(auto* decl: decls) {
            if(auto* inner = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
                if(inner->isInline() || inner->isAnonymousNamespace()) {
                    enter(inner);
                }
            } else if(auto* directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
                if(!directive->getNominatedNamespace()->isAnonymousNamespace()) {
                    return true;
                }
            } else if(auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
                if(linkage->getLanguage() != clang::LinkageSpecLanguageIDs::CXX) {
                    return true;
                }
                pending.push_back(linkage);
            } else if(auto* using_enum = llvm::dyn_cast<clang::UsingEnumDecl>(decl)) {
                // The index files an enumeration's enumerators under it.
                if(!space->Encloses(using_enum->getEnumDecl()->getDeclContext())) {
                    return true;
                }
            } else {
                return true;
            }
        }
    }
    return false;
}

void read_names(clang::DeclContext* space) {
    llvm::SmallPtrSet<clang::DeclContext*, 8> read;
    llvm::SmallVector<clang::DeclContext*, 8> pending = {space->getPrimaryContext()};
    while(!pending.empty()) {
        auto* context = pending.pop_back_val();
        if(!read.insert(context).second) {
            continue;
        }
        context->lookups();
        for(auto* directive: context->using_directives()) {
            pending.push_back(directive->getNominatedNamespace()->getPrimaryContext());
        }
    }
}

}  // namespace clice::feature
