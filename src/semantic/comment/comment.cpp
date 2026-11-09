module clice;

import :compile.compilation;
import :semantic.comment;
import :semantic.decls;

namespace clice {

AttachedComments attached_comments(CompilationUnit& unit, const clang::Decl* decl) {
    if(decl->isImplicit() || llvm::isa<clang::NamespaceDecl,
                                       clang::ParmVarDecl,
                                       clang::TemplateTypeParmDecl,
                                       clang::NonTypeTemplateParmDecl,
                                       clang::TemplateTemplateParmDecl>(decl)) {
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
    auto run = attached_comments(
        content,
        unit.comments(fid),
        {.range = extent,
         .trailing = llvm::isa<clang::FieldDecl, clang::EnumConstantDecl, clang::VarDecl>(decl)});
    return {fid, content, run};
}

llvm::SmallVector<DocumentedParameter> documented_parameters(CompilationUnit& unit,
                                                             const clang::Decl* decl) {
    const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    const auto* concept_decl = llvm::dyn_cast<clang::ConceptDecl>(decl);
    const auto* templates =
        concept_decl ? concept_decl->getTemplateParameters() : decl->getDescribedTemplateParams();
    llvm::SmallVector<DocumentedParameter> result;
    if(!function && !templates) {
        return result;
    }
    auto found = attached_comments(unit, decl);
    DocCommandScanner scanner;
    for(const auto& comment: found.comments) {
        auto text = found.content.slice(comment.range.begin, comment.range.end);
        if(!is_doxygen_comment(text)) {
            scanner.end_run();
            continue;
        }
        for(const auto& part: scanner.scan(text)) {
            using Kind = DocCommandScanner::Part::Kind;
            auto name = text.slice(part.begin, part.end);
            auto named = [&](const clang::NamedDecl* parameter) {
                return parameter->getName() == name;
            };
            const clang::NamedDecl* parameter = nullptr;
            if(part.kind == Kind::Parameter && function) {
                auto parameters = function->parameters();
                if(auto it = ranges::find_if(parameters, named); it != function->param_end()) {
                    parameter = *it;
                }
            } else if(part.kind == Kind::TemplateParameter && templates) {
                if(auto it = ranges::find_if(*templates, named); it != templates->end()) {
                    parameter = *it;
                }
            }
            if(parameter) {
                auto offset = comment.range.begin + static_cast<std::uint32_t>(part.begin);
                result.push_back(
                    {.parameter = parameter, .location = unit.create_location(found.file, offset)});
            }
        }
    }
    return result;
}

}  // namespace clice
