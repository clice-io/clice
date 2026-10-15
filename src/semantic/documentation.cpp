module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :semantic.decls;
import :semantic.documentation;
import :support.text;
import :syntax.doc_comment;
import :syntax.documentation;
import :syntax.lexical_scan;

namespace clice {

namespace {

/// The comments documenting this one declaration, and the text of its
/// file they are in.
std::pair<llvm::StringRef, llvm::ArrayRef<LexicalInfo::Comment>>
    documenting(CompilationUnit& unit, const clang::Decl* decl) {
    auto range = decls::written_range(decl);
    if(decl->isImplicit() || range.isInvalid()) {
        return {};
    }
    /// The declaration's range starts after the attributes leading it, whose
    /// arguments may hold anything, as a `;` in a deprecation message. One
    /// `#pragma clang attribute` applies is written on the pragma's line.
    auto& sources = decl->getASTContext().getSourceManager();
    for(const auto* attr: decl->attrs()) {
        auto begin = attr->getRange().getBegin();
        if(attr->isImplicit() || attr->isInherited() || begin.isInvalid() ||
           !sources.isBeforeInTranslationUnit(begin, range.getBegin())) {
            continue;
        }
        auto [file, offset] = sources.getDecomposedExpansionLoc(begin);
        auto content = sources.getBufferData(file);
        if(!content.substr(line_begin(content, offset)).ltrim(" \t").starts_with("#")) {
            range.setBegin(begin);
        }
    }
    auto [fid, extent] = unit.produced_range(range);
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
    return {content, run};
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

    /// Namespaces often have too many redecls for any particular redecl
    /// comment to be useful, and file headers pass for their comments.
    if(llvm::isa<clang::NamespaceDecl,
                 clang::ParmVarDecl,
                 clang::TemplateTypeParmDecl,
                 clang::NonTypeTemplateParmDecl,
                 clang::TemplateTemplateParmDecl>(decl)) {
        return {};
    }

    DocCommentChoice choice;
    for(const auto* redecl: decl->getCanonicalDecl()->redecls()) {
        auto [content, run] = documenting(unit, redecl);
        if(choice.offer(content, run)) {
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
