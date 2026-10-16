module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :semantic.decls;
import :semantic.documentation;
import :syntax.doc_comment;
import :syntax.documentation;
import :syntax.lexical_scan;

namespace clice {

namespace {

/// The comments documenting this one declaration, and the file they are in.
struct Documenting {
    clang::FileID file;
    llvm::StringRef content;
    llvm::ArrayRef<LexicalInfo::Comment> comments;
};

Documenting documenting(CompilationUnit& unit, const clang::Decl* decl) {
    if(decl->isImplicit()) {
        return {};
    }
    auto [fid, extent] = unit.produced_range(decls::written_range(decl));
    // The specialization Sema matches an explicit specialization with takes
    // the explicit one's name but keeps the template's extent.
    auto [name_fid, name] = unit.produced_range(decl->getLocation());
    if(name_fid != fid || name.begin < extent.begin || name.begin > extent.end) {
        return {};
    }
    auto content = unit.file_content(fid);
    auto run = documenting_comments(
        content,
        unit.comments(fid),
        {.range = extent,
         .trailing = llvm::isa<clang::FieldDecl, clang::EnumConstantDecl, clang::VarDecl>(decl)});
    return {fid, content, run};
}

/// Namespaces often have too many redecls for any particular redecl
/// comment to be useful, and file headers pass for their comments.
bool takes_comments(const clang::Decl* decl) {
    return !llvm::isa<clang::NamespaceDecl,
                      clang::ParmVarDecl,
                      clang::TemplateTypeParmDecl,
                      clang::NonTypeTemplateParmDecl,
                      clang::TemplateTemplateParmDecl>(decl);
}

}  // namespace

std::string doc_comment(CompilationUnit& unit, const clang::NamedDecl* decl, bool inherit) {
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

    if(!takes_comments(decl)) {
        return {};
    }

    DocCommentChoice choice;
    for(const auto* redecl: decl->getCanonicalDecl()->redecls()) {
        auto found = documenting(unit, redecl);
        if(choice.offer(found.content, found.comments)) {
            break;
        }
    }

    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
       method && inherit && choice.empty()) {
        for(const auto* overridden: method->overridden_methods()) {
            if(auto text = doc_comment(unit, overridden, inherit); !text.empty()) {
                return text;
            }
        }
    }
    return std::move(choice).take();
}

auto doc_comment_range(CompilationUnit& unit, const clang::Decl* decl)
    -> std::optional<std::pair<clang::FileID, LocalSourceRange>> {
    if(!takes_comments(decl)) {
        return std::nullopt;
    }
    auto found = documenting(unit, decl);
    if(found.comments.empty()) {
        return std::nullopt;
    }
    return std::pair(found.file,
                     LocalSourceRange{found.comments.front().range.begin,
                                      found.comments.back().range.end});
}

std::string doc_comment(CompilationUnit& unit, const clang::MacroInfo* macro) {
    auto [fid, extent] = *unit.written_range(
        clang::SourceRange(macro->getDefinitionLoc(), macro->getDefinitionEndLoc()));
    auto content = unit.file_content(fid);
    auto run = documenting_comments(content, unit.comments(fid), documented_macro(content, extent));
    return comment_text(content, run);
}

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
        for(auto part: llvm::drop_begin(copied->parts)) {
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
            llvm::erase_if(found,
                           [&](const clang::NamedDecl* candidate) { return !declares(candidate); });
        }
        return found;
    }
    return {};
}

}  // namespace clice
