module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.documentation;
import :semantic.decls;
import :syntax.doc_comment;
import :syntax.documentation;

namespace clice {

namespace {

struct Found {
    std::string text;
    bool doxygen = false;
};

/// The comment documenting this one declaration.
Found comment_of(CompilationUnitRef unit, const clang::Decl* decl) {
    auto range = decls::written_range(decl);
    if(decl->isImplicit() || range.isInvalid()) {
        return {};
    }
    auto [fid, extent] = unit.decompose_expansion_range(range);
    auto content = unit.file_content(fid);
    auto run = documenting_comments(
        content,
        unit.comments(fid),
        {.range = extent,
         .trailing = llvm::isa<clang::FieldDecl, clang::EnumConstantDecl, clang::VarDecl>(decl)});
    if(run.empty()) {
        return {};
    }
    auto first = content.slice(run.front().range.begin, run.front().range.end);
    return {comment_text(content, run), is_doc_comment(first)};
}

}  // namespace

std::string doc_comment(CompilationUnitRef unit, const clang::NamedDecl* decl, bool inherit) {
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

    /// Namespaces often have too many redecls for any particular redecl
    /// comment to be useful, and file headers pass for their comments.
    if(llvm::isa<clang::NamespaceDecl,
                 clang::ParmVarDecl,
                 clang::TemplateTypeParmDecl,
                 clang::NonTypeTemplateParmDecl,
                 clang::TemplateTemplateParmDecl>(decl)) {
        return {};
    }

    std::string fallback;
    for(const auto* redecl: decl->getCanonicalDecl()->redecls()) {
        auto found = comment_of(unit, redecl);
        if(found.doxygen) {
            return std::move(found.text);
        }
        if(fallback.empty()) {
            fallback = std::move(found.text);
        }
    }

    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
       method && inherit && fallback.empty()) {
        for(const auto* overridden: method->overridden_methods()) {
            if(auto text = doc_comment(unit, overridden, inherit); !text.empty()) {
                return text;
            }
        }
    }
    return fallback;
}

std::string doc_comment(CompilationUnitRef unit, const clang::MacroInfo* macro) {
    auto [fid, extent] = unit.decompose_range(
        clang::SourceRange(macro->getDefinitionLoc(), macro->getDefinitionEndLoc()));
    auto content = unit.file_content(fid);
    auto run = documenting_comments(content, unit.comments(fid), documented_macro(content, extent));
    return comment_text(content, run);
}

llvm::SmallVector<const clang::NamedDecl*> copied_doc_targets(CompilationUnitRef unit,
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
        auto [fid, range] =
            unit.decompose_expansion_range(function->getNameInfo().getSourceRange());
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
