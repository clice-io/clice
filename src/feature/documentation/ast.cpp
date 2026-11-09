module clice;

import :compile.compilation;
import :feature.documentation.reading;
import :feature.documentation;
import :semantic.comment;
import :semantic.decls;

namespace clice::feature {

namespace {

/// The text (comment_text) of the comment documenting `decl`: of its
/// declarations, the one CommentChoice chooses; a template's instantiation
/// and its members read the template's. With `inherit`, a method
/// overriding another without one takes that method's.
std::string comment_of(CompilationUnit& unit, const clang::NamedDecl* decl, bool inherit) {
    /// Chase the pattern to a fixed point: a comparison operator instantiated
    /// from a spaceship operator that is itself a template instantiation
    /// needs two hops. Termination rests on clang's instantiation graph
    /// being acyclic — every hop moves strictly toward a written pattern.
    while(const auto* pattern = decls::instantiated_from(decl)) {
        if(pattern == decl) {
            break;
        }
        decl = pattern;
    }
    if(const auto* template_decl = llvm::dyn_cast<clang::TemplateDecl>(decl);
       template_decl && template_decl->getTemplatedDecl()) {
        decl = template_decl->getTemplatedDecl();
    }

    CommentChoice choice;
    for(const auto* redecl: decl->getCanonicalDecl()->redecls()) {
        auto found = attached_comments(unit, redecl);
        if(choice.offer(found.content, found.comments)) {
            break;
        }
    }

    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
       method && inherit && choice.empty()) {
        for(const auto* overridden: method->overridden_methods()) {
            if(auto text = comment_of(unit, overridden, inherit); !text.empty()) {
                return text;
            }
        }
    }
    return std::move(choice).take();
}

/// The text of the comment documenting a macro: above its `#define`, or
/// after it on its line.
std::string comment_of(CompilationUnit& unit, const clang::MacroInfo* macro) {
    auto [fid, extent] = *unit.written_range(
        clang::SourceRange(macro->getDefinitionLoc(), macro->getDefinitionEndLoc()));
    auto content = unit.file_content(fid);
    auto run = attached_comments(content, unit.comments(fid), documented_macro(content, extent));
    return comment_text(content, run);
}

/// The declarations a `@copydoc` in the comment of `decl` may name
/// (parse_copied_name): looked up from `decl`'s scope outward, or in the
/// global scope, a qualified name part by part; of several, the functions
/// declaring the parameters it lists, when it lists them.
llvm::SmallVector<const clang::NamedDecl*> copied_doc_targets(CompilationUnit& unit,
                                                              const clang::NamedDecl* decl,
                                                              llvm::StringRef name) {
    auto copied = parse_copied_name(name);
    if(!copied) {
        return {};
    }

    auto& context = decl->getASTContext();
    auto lookup = [&](const clang::DeclContext* scope, llvm::StringRef part) {
        return scope->lookup(clang::DeclarationName(&context.Idents.get(part)));
    };
    auto scope_of = [](const clang::NamedDecl* found) -> const clang::DeclContext* {
        if(const auto* class_template = llvm::dyn_cast<clang::ClassTemplateDecl>(found)) {
            return class_template->getTemplatedDecl();
        }
        return llvm::dyn_cast<clang::DeclContext>(found);
    };
    auto declares = [&](const clang::NamedDecl* candidate) {
        const auto* function = candidate->getAsFunction();
        if(!function) {
            return false;
        }
        auto [fid, range] = unit.produced_range(function->getNameInfo().getSourceRange());
        return declares_parameters(unit.file_content(fid), range.end, *copied->parameters);
    };

    const clang::DeclContext* scope =
        copied->absolute ? context.getTranslationUnitDecl() : decl->getDeclContext();
    for(; scope; scope = scope->getParent()) {
        auto first = lookup(scope, copied->parts.front());
        llvm::SmallVector<const clang::NamedDecl*> found(first.begin(), first.end());
        for(auto part: views::drop(copied->parts, 1)) {
            llvm::SmallVector<const clang::NamedDecl*> members;
            for(const auto* candidate: found) {
                if(const auto* inner = scope_of(candidate)) {
                    auto result = lookup(inner, part);
                    members.append(result.begin(), result.end());
                }
            }
            found = std::move(members);
        }
        if(found.empty()) {
            continue;
        }
        if(copied->parameters && found.size() > 1) {
            found.erase(ranges::remove_if(
                            found,
                            [&](const clang::NamedDecl* candidate) { return !declares(candidate); })
                            .begin(),
                        found.end());
        }
        return found;
    }
    return {};
}

Documentation decl_documentation(CompilationUnit& unit,
                                 const clang::NamedDecl* decl,
                                 const DocumentationOptions& options,
                                 unsigned depth) {
    return read_copying(
        comment_of(unit, decl, options.inherit),
        options,
        depth,
        [&](llvm::StringRef name) { return copied_doc_targets(unit, decl, name); },
        [&](const clang::NamedDecl* target, unsigned next) {
            return decl_documentation(unit, target, options, next);
        });
}

}  // namespace

auto documentation(CompilationUnit& unit,
                   const clang::NamedDecl* decl,
                   const DocumentationOptions& options) -> Documentation {
    return decl_documentation(unit, decl, options, 0);
}

auto documentation(CompilationUnit& unit,
                   const clang::MacroInfo* macro,
                   const DocumentationOptions& options) -> Documentation {
    return read(comment_of(unit, macro), options, nothing_copied);
}

}  // namespace clice::feature
