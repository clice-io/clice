module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.documentation;
import :semantic.decls;
import :syntax.doc_comment;

namespace clice {

namespace {

struct Found {
    std::string text;
    bool doxygen = false;
};

/// The comment documenting this one declaration.
Found comment_of(CompilationUnitRef unit, const clang::Decl* decl) {
    auto range = decl->getSourceRange();
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

llvm::SmallVector<const clang::NamedDecl*> copied_doc_targets(const clang::NamedDecl* decl,
                                                              llvm::StringRef name) {
    // Doxygen names a member `Class::member` or `Class#member`.
    llvm::SmallVector<llvm::StringRef> parts;
    name = name.take_until([](char c) { return c == '('; }).trim();
    name.consume_front("::");
    for(auto part: llvm::split(name, "::")) {
        for(auto piece: llvm::split(part, '#')) {
            if(piece.trim().empty()) {
                return {};
            }
            parts.push_back(piece.trim());
        }
    }
    if(parts.empty()) {
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

    for(const auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
        auto first = lookup(scope, parts.front());
        llvm::SmallVector<const clang::NamedDecl*> found(first.begin(), first.end());
        for(auto part: llvm::drop_begin(parts)) {
            llvm::SmallVector<const clang::NamedDecl*> members;
            for(const auto* candidate: found) {
                if(const auto* inner = scope_of(candidate)) {
                    auto result = lookup(inner, part);
                    members.append(result.begin(), result.end());
                }
            }
            found = std::move(members);
        }
        if(!found.empty()) {
            return found;
        }
    }
    return {};
}

}  // namespace clice
