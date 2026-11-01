module clice;

import :feature.code_completion.names;
import :feature.code_completion;
import :index.search;

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

}  // namespace

clang::DeclContext* load_names(clang::Sema& sema,
                               const index::CompletionScope& scope,
                               llvm::ArrayRef<ScopeCandidate> candidates) {
    auto& ast = sema.getASTContext();
    clang::DeclContext* space = nullptr;
    if(scope.qualified()) {
        space = qualified_namespace(sema, scope);
        if(!space) {
            return nullptr;
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
            read_names(space);
        }
    }
    for(const auto& candidate: candidates) {
        // Its identifier comes with the macro the PCH defines by its name.
        clang::LookupResult found(sema,
                                  &ast.Idents.get(candidate.name),
                                  {},
                                  clang::Sema::LookupOrdinaryName);
        found.suppressDiagnostics();
        if(space) {
            sema.LookupQualifiedName(found, space);
        } else if(!candidate.macro) {
            sema.LookupName(found, sema.getCurScope());
        }
    }
    return space;
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
