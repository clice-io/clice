#include <array>
#include <bitset>
#include <string>
#include <utility>
#include <vector>

#include "command/command.h"
#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/display.h"
#include "semantic/semantics.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Driver/Types.h"

namespace clice::feature {

namespace action {

namespace {

constexpr std::array specs = {
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = define,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = define_missing,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = define_missing,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = implement_pure_virtuals,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = memberwise_constructor,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = reorder_definitions,
         .format = false},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = reorder_definitions,
         .format = false},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::SwitchStmt>(),
         .reach = Reach::Ancestors,
         .run = populate_switch,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::TypeLoc>(),
         .reach = Reach::Innermost,
         .run = expand_deduced_type,
         .format = false},
};

void format_actions(CompilationUnitRef unit, std::vector<CodeAction>& actions, std::size_t from) {
    auto path = unit.file_path(unit.main_file());
    for(auto& action: llvm::drop_begin(actions, from)) {
        if(!action.edits.empty()) {
            action.edits = format_edits(path, unit.main_content(), std::move(action.edits));
        }
    }
}

/// Walk the ancestor chain of the selection's innermost node and offer
/// each spec its innermost matching node, once.
void enumerate(CompilationUnitRef unit,
               const SelectionTree& tree,
               LocalSourceRange selection,
               bool main_is_header,
               std::vector<CodeAction>& out) {
    const auto* innermost = tree.common_ancestor();
    if(!innermost) {
        return;
    }

    std::bitset<specs.size()> fired;
    for(const auto* node = innermost; node; node = node->parent) {
        auto kind = node->data.getNodeKind();
        for(auto [index, spec]: llvm::enumerate(specs)) {
            if(fired[index] || (spec.reach == Reach::Innermost && node != innermost) ||
               !spec.anchor.isBaseOf(kind)) {
                continue;
            }
            fired[index] = true;
            auto before = out.size();
            spec.run(Context{unit, selection, *node, main_is_header}, out);
            if(spec.format) {
                format_actions(unit, out, before);
            }
        }
    }
}

bool is_header(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    return type == types::TY_CHeader || type == types::TY_CXXHeader;
}

}  // namespace

std::optional<LocalSourceRange> main_range(CompilationUnitRef unit, clang::SourceRange range) {
    if(range.isInvalid() || !range.getBegin().isFileID() || !range.getEnd().isFileID()) {
        return std::nullopt;
    }
    auto main = unit.main_file();
    if(unit.file_id(range.getBegin()) != main || unit.file_id(range.getEnd()) != main) {
        return std::nullopt;
    }
    return unit.decompose_range(range).second;
}

std::optional<llvm::StringRef> spelled_text(CompilationUnitRef unit, clang::SourceRange range) {
    if(range.isInvalid() || !range.getBegin().isFileID() || !range.getEnd().isFileID()) {
        return std::nullopt;
    }
    auto fid = unit.file_id(range.getBegin());
    if(fid != unit.file_id(range.getEnd()) || unit.is_builtin_file(fid)) {
        return std::nullopt;
    }
    auto local = unit.decompose_range(range).second;
    return unit.file_content(fid).substr(local.begin, local.length());
}

std::uint32_t line_begin(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.rfind('\n', offset);
    return newline == llvm::StringRef::npos ? 0 : static_cast<std::uint32_t>(newline + 1);
}

std::uint32_t line_end(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.find('\n', offset);
    return newline == llvm::StringRef::npos ? static_cast<std::uint32_t>(content.size())
                                            : static_cast<std::uint32_t>(newline + 1);
}

llvm::StringRef line_indent(llvm::StringRef content, std::uint32_t offset) {
    auto line = content.substr(line_begin(content, offset));
    return line.take_while([](char c) { return c == ' ' || c == '\t'; });
}

namespace {

/// Declarators bind to their type: `T*`, `T&`, never `T *`.
std::string bind_declarators(std::string text) {
    for(auto at = text.find(" *"); at != std::string::npos; at = text.find(" *", at)) {
        text.erase(at, 1);
    }
    for(auto at = text.find(" &"); at != std::string::npos; at = text.find(" &", at)) {
        text.erase(at, 1);
    }
    return text;
}

/// A record as a qualifier component: its name with the arguments of a
/// specialization as written, or the parameters of a class template as
/// arguments.
std::string record_component(const clang::RecordDecl* record) {
    std::string name = display::name_of(record, {.qualified = false});
    if(auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
        if(llvm::isa<clang::ClassTemplateSpecializationDecl>(cxx)) {
            return name + bind_declarators(display::template_args(*cxx));
        }
        if(auto* described = cxx->getDescribedClassTemplate()) {
            llvm::raw_string_ostream os(name);
            os << '<';
            for(auto [index, param]: llvm::enumerate(*described->getTemplateParameters())) {
                if(index) {
                    os << ", ";
                }
                os << param->getName();
                if(param->isParameterPack()) {
                    os << "...";
                }
            }
            os << '>';
        }
    }
    return name;
}

}  // namespace

std::string qualifier_at(const clang::DeclContext* target, const clang::DeclContext* from) {
    llvm::SmallVector<std::string, 4> components;
    for(const auto* context = target; context && !context->isTranslationUnit();
        context = context->getParent()) {
        if(from && context->Encloses(from)) {
            break;
        }
        if(auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            if(!ns->isAnonymousNamespace() && !ns->isInline()) {
                components.push_back(ns->getNameAsString());
            }
        } else if(auto* record = llvm::dyn_cast<clang::RecordDecl>(context)) {
            components.push_back(record_component(record));
        } else if(auto* en = llvm::dyn_cast<clang::EnumDecl>(context)) {
            if(en->isScoped()) {
                components.push_back(en->getNameAsString());
            }
        } else if(!llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context)) {
            // A function-local context: nothing outside it can qualify
            // its members.
            break;
        }
    }
    std::string qualifier;
    for(auto& component: llvm::reverse(components)) {
        qualifier += component;
        qualifier += "::";
    }
    return qualifier;
}

std::string name_at(const clang::NamedDecl* decl, const clang::DeclContext* from) {
    return qualifier_at(decl->getDeclContext(), from) +
           display::name_of(decl, {.qualified = false});
}

namespace {

/// Erase every occurrence of `prefix` that starts a qualified name: at
/// the beginning or after a character no identifier or qualifier ends
/// with.
void strip_qualifier(std::string& text, llvm::StringRef prefix) {
    for(auto at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at)) {
        char before = at == 0 ? ' ' : text[at - 1];
        if(at == 0 || (!llvm::isAlnum(before) && before != '_' && before != ':')) {
            text.erase(at, prefix.size());
        } else {
            at += 1;
        }
    }
}

}  // namespace

std::string type_name(clang::ASTContext& context,
                      clang::QualType type,
                      const clang::DeclContext* from) {
    clang::PrintingPolicy policy = context.getPrintingPolicy();
    policy.SuppressScope = false;
    policy.SuppressUnwrittenScope = true;
    policy.AnonymousTagNameStyle =
        std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
    policy.FullyQualifiedName = true;
    // A deduced type keeps its `auto` as sugar, one layer per deduction
    // chained through; the qualified-name printer takes only what lies
    // beneath.
    for(const auto* deduced = type->getAs<clang::DeducedType>(); deduced && deduced->isDeduced();
        deduced = type->getAs<clang::DeducedType>()) {
        type = context.getQualifiedType(deduced->getDeducedType(), type.getLocalQualifiers());
    }
    auto printed = clang::TypeName::getFullyQualifiedName(type, context, policy);

    llvm::SmallVector<const clang::NamespaceDecl*, 4> namespaces;
    for(const auto* context_ = from; context_; context_ = context_->getParent()) {
        auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context_);
        if(ns && !ns->isAnonymousNamespace() && !ns->isInline()) {
            namespaces.push_back(ns);
        }
    }
    llvm::SmallVector<std::string, 4> prefixes;
    std::string chain;
    for(const auto* ns: llvm::reverse(namespaces)) {
        chain += ns->getName();
        chain += "::";
        prefixes.push_back(chain);
    }
    for(const auto& prefix: llvm::reverse(prefixes)) {
        strip_qualifier(printed, prefix);
    }
    return bind_declarators(std::move(printed));
}

std::string template_head(CompilationUnitRef unit, const clang::TemplateParameterList* params) {
    auto& context = unit.context();
    std::string head;
    llvm::raw_string_ostream os(head);
    os << "template <";
    for(auto [index, param]: llvm::enumerate(*params)) {
        if(index) {
            os << ", ";
        }
        if(auto* type = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param)) {
            os << (type->wasDeclaredWithTypename() ? "typename" : "class");
            if(type->isParameterPack()) {
                os << "...";
            }
        } else if(auto* value = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param)) {
            os << type_name(context, value->getType());
            if(value->isParameterPack()) {
                os << "...";
            }
        } else if(auto* tmpl = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param)) {
            os << template_head(unit, tmpl->getTemplateParameters()) << " class";
            if(tmpl->isParameterPack()) {
                os << "...";
            }
        }
        if(!param->getName().empty()) {
            os << ' ' << param->getName();
        }
    }
    os << '>';
    if(auto* requires_clause = params->getRequiresClause()) {
        if(auto text = spelled_text(unit, requires_clause->getSourceRange())) {
            os << " requires " << *text;
        }
    }
    return head;
}

std::string template_heads(CompilationUnitRef unit,
                           const clang::Decl* decl,
                           const clang::DeclContext* from) {
    llvm::SmallVector<const clang::TemplateParameterList*, 2> lists;
    for(const auto* context = decl->getDeclContext(); context && !context->isTranslationUnit();
        context = context->getParent()) {
        if(from && context->Encloses(from)) {
            break;
        }
        auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
        if(!record) {
            continue;
        }
        if(auto* partial = llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(record)) {
            lists.push_back(partial->getTemplateParameters());
        } else if(auto* described = record->getDescribedClassTemplate()) {
            lists.push_back(described->getTemplateParameters());
        }
    }
    std::string heads;
    for(const auto* params: llvm::reverse(lists)) {
        heads += template_head(unit, params);
        heads += '\n';
    }
    return heads;
}

std::optional<BodyInsertion> class_body_insertion(CompilationUnitRef unit,
                                                  const clang::CXXRecordDecl* record) {
    auto brace = main_range(unit, record->getBraceRange().getEnd());
    if(!brace) {
        return std::nullopt;
    }
    auto content = unit.main_content();
    auto begin = line_begin(content, brace->begin);
    bool own_line = content.substr(begin, brace->begin - begin).trim().empty();

    std::string indent;
    for(const auto* member: record->decls()) {
        if(member->isImplicit()) {
            continue;
        }
        if(auto range = main_range(unit, member->getBeginLoc())) {
            indent = line_indent(content, range->begin).str();
            break;
        }
    }
    if(indent.empty()) {
        auto record_line = main_range(unit, record->getBeginLoc());
        indent =
            (record_line ? line_indent(content, record_line->begin) : llvm::StringRef()).str() +
            "    ";
    }
    return BodyInsertion{
        .offset = own_line ? begin : brace->begin,
        .indent = std::move(indent),
        .break_before = !own_line,
    };
}

bool ends_public(const clang::CXXRecordDecl* record) {
    auto access =
        record->getTagKind() == clang::TagTypeKind::Class ? clang::AS_private : clang::AS_public;
    for(const auto* member: record->decls()) {
        if(auto* specifier = llvm::dyn_cast<clang::AccessSpecDecl>(member)) {
            access = specifier->getAccess();
        }
    }
    return access == clang::AS_public;
}

void for_each_file_scope_decl(CompilationUnitRef unit,
                              llvm::function_ref<void(const clang::Decl*)> visit) {
    auto entries = unit.semantics().node_entries();
    for(std::uint32_t i = 0; i < entries.size();) {
        const auto& node = entries[i];
        if(!node.node.is_ast()) {
            break;
        }
        const auto* decl = node.node.get<clang::Decl>();
        if(!decl || node.flags.in_instantiation) {
            i = node.subtree_end;
            continue;
        }
        if(llvm::isa<clang::NamespaceDecl, clang::LinkageSpecDecl, clang::ExportDecl>(decl)) {
            i += 1;
            continue;
        }
        visit(decl);
        i = node.subtree_end;
    }
}

const clang::Decl* written_declaration(const clang::FunctionDecl* decl) {
    if(auto* described = decl->getDescribedFunctionTemplate()) {
        return described;
    }
    return decl;
}

std::vector<const clang::FunctionDecl*>
    out_of_line_definitions(CompilationUnitRef unit, const clang::CXXRecordDecl* record) {
    std::vector<const clang::FunctionDecl*> definitions;
    const auto* canonical = record->getCanonicalDecl();
    for_each_file_scope_decl(unit, [&](const clang::Decl* decl) {
        if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
            decl = described->getTemplatedDecl();
        }
        auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
        if(method && method->isThisDeclarationADefinition() && method->isOutOfLine() &&
           method->getParent()->getCanonicalDecl() == canonical) {
            definitions.push_back(method);
        }
    });
    return definitions;
}

}  // namespace action

auto code_action_kind_name(CodeActionKind kind) -> llvm::StringRef {
    switch(kind) {
        case CodeActionKind::QuickFix: return "quickfix";
        case CodeActionKind::Refactor: return "refactor";
        case CodeActionKind::RefactorInline: return "refactor.inline";
        case CodeActionKind::RefactorRewrite: return "refactor.rewrite";
    }
    std::unreachable();
}

auto code_actions(CompilationUnitRef unit, LocalSourceRange selection) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;
    action::add_include(unit, selection, out);

    bool main_is_header = action::is_header(unit.file_path(unit.main_file()));
    SelectionTree::create_each(unit, selection, [&](SelectionTree tree) {
        auto before = out.size();
        action::enumerate(unit, tree, selection, main_is_header, out);
        return out.size() > before;
    });

    auto before = out.size();
    action::expand_macro(unit, selection, out);
    action::format_actions(unit, out, before);
    return out;
}

auto assemble_definitions(const DefineRequest& request,
                          llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::string> {
    std::string text;
    for(const auto& piece: request.pieces) {
        if(defined_elsewhere(piece.entity)) {
            continue;
        }
        if(!text.empty()) {
            text += '\n';
        }
        text += piece.text;
    }
    if(text.empty()) {
        return std::nullopt;
    }
    return text;
}

}  // namespace clice::feature
