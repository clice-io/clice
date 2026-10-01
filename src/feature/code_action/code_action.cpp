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
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TypeLoc.h"

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
            spec.run(Context{unit, *node, main_is_header}, out);
            if(spec.format) {
                format_actions(unit, out, before);
            }
        }
    }
}

/// Declarators bind to their type: `T* p`, `const T& r`, never `T *p`.
std::string bind_declarators(std::string text) {
    for(auto at = text.find(' '); at != std::string::npos; at = text.find(' ', at + 1)) {
        auto next = at + 1;
        if(next >= text.size() || (text[next] != '*' && text[next] != '&')) {
            continue;
        }
        text.erase(at, 1);
        auto after = text.find_first_not_of("*&", at);
        if(after != std::string::npos && (llvm::isAlnum(text[after]) || text[after] == '_')) {
            text.insert(after, 1, ' ');
        }
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

/// The entity a found declaration denotes: through using-declarations and
/// namespace aliases, a class template as its pattern and an injected
/// class name as its class.
const clang::Decl* entity_of(const clang::NamedDecl* decl) {
    decl = decl->getUnderlyingDecl();
    if(auto* alias = llvm::dyn_cast<clang::NamespaceAliasDecl>(decl)) {
        decl = alias->getNamespace();
    } else if(auto* described = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
        decl = described->getTemplatedDecl();
    } else if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl);
              record && record->isInjectedClassName()) {
        decl = llvm::cast<clang::CXXRecordDecl>(record->getDeclContext());
    }
    return decl->getCanonicalDecl();
}

/// The entities a lookup found; normalized, sorted and unique, they
/// compare as sets.
using Entities = llvm::SmallVector<const clang::Decl*, 2>;

void add_found(Entities& entities, clang::DeclContext::lookup_result found) {
    for(const auto* decl: found) {
        entities.push_back(entity_of(decl));
    }
}

Entities normalized(Entities entities) {
    llvm::sort(entities);
    entities.erase(llvm::unique(entities), entities.end());
    return entities;
}

/// The name an identifier spells; empty when no declaration can have it.
clang::DeclarationName declaration_name(clang::ASTContext& context, llvm::StringRef identifier) {
    auto found = context.Idents.find(identifier);
    if(found == context.Idents.end()) {
        return {};
    }
    return found->getValue();
}

/// Qualified lookup into a namespace or the global scope: its members,
/// else those of the namespaces its using-directives nominate.
Entities lookup_qualified(const clang::DeclContext* scope, clang::DeclarationName name) {
    Entities entities;
    add_found(entities, scope->lookup(name));
    if(entities.empty()) {
        for(const auto* directive: scope->using_directives()) {
            add_found(entities, directive->getNominatedNamespace()->lookup(name));
        }
    }
    return normalized(std::move(entities));
}

/// The members a scope declares under `name`; a class declaring none
/// inherits its bases'. Dependent bases stay unsearched, as unqualified
/// lookup in a template leaves them.
void add_members(Entities& entities, const clang::DeclContext* scope, clang::DeclarationName name) {
    auto found = scope->lookup(name);
    if(!found.empty()) {
        add_found(entities, found);
        return;
    }
    const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(scope);
    if(!record || !record->hasDefinition()) {
        return;
    }
    for(const auto& base: record->getDefinition()->bases()) {
        if(const auto* base_record = base.getType()->getAsCXXRecordDecl()) {
            add_members(entities, base_record, name);
        }
    }
}

/// What an unqualified `name` written in `from` finds: everything the
/// innermost scope declaring it offers — its template parameters, its
/// members and those of the namespaces its using-directives nominate.
/// Declarations later in the file count as seen, which only ever keeps
/// a qualifier that was not needed.
Entities lookup_unqualified(const clang::DeclContext* from, clang::DeclarationName name) {
    Entities entities;
    for(const auto* scope = from; scope && entities.empty(); scope = scope->getParent()) {
        if(scope->isTransparentContext()) {
            continue;
        }
        if(const auto* params = llvm::cast<clang::Decl>(scope)->getDescribedTemplateParams()) {
            for(const auto* param: *params) {
                if(param->getDeclName() == name) {
                    entities.push_back(param->getCanonicalDecl());
                }
            }
        }
        add_members(entities, scope, name);
        for(const auto* directive: scope->using_directives()) {
            add_found(entities, directive->getNominatedNamespace()->lookup(name));
        }
    }
    return normalized(std::move(entities));
}

/// The declarations a type spells by name. A type is unnameable at `from`
/// when one of them is: another function's local type, a member type
/// `from` has no access to (friendship aside), an unnamed or closure
/// type. Every name printed qualified from the global scope is recorded
/// by its first component.
class SpelledNames : public clang::RecursiveASTVisitor<SpelledNames> {
public:
    explicit SpelledNames(const clang::DeclContext* from) : from(from) {}

    bool VisitTagType(clang::TagType* type) {
        auto* decl = type->getDecl();
        if(auto* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            if(!TraverseTemplateArguments(specialization->getTemplateArgs().asArray())) {
                return false;
            }
        }
        return name(decl);
    }

    bool VisitTypedefType(clang::TypedefType* type) {
        return name(type->getDecl());
    }

    bool VisitUsingType(clang::UsingType* type) {
        return name(type->getDecl());
    }

    bool VisitTemplateSpecializationType(clang::TemplateSpecializationType* type) {
        auto* decl = type->getTemplateName().getAsTemplateDecl();
        return !decl || name(decl);
    }

    bool VisitBuiltinType(clang::BuiltinType* type) {
        null_pointer |= type->getKind() == clang::BuiltinType::NullPtr;
        return true;
    }

    bool VisitPredefinedSugarType(clang::PredefinedSugarType* type) {
        predefined.push_back(type);
        return true;
    }

    bool nameable = true;
    /// The first component of each name printed from the global scope,
    /// with the entity it denotes there.
    llvm::SmallVector<std::pair<llvm::StringRef, const clang::Decl*>, 4> roots;
    /// `std::nullptr_t` is printed for the type of `nullptr`, declared or not.
    bool null_pointer = false;
    /// The compiler's names for the types of `sizeof` and pointer
    /// differences (`__size_t`), which no user can spell.
    llvm::SmallVector<const clang::PredefinedSugarType*, 1> predefined;

private:
    bool name(const clang::NamedDecl* decl) {
        if(decl->isTemplateParameter()) {
            return true;
        }
        const clang::NamedDecl* root = decl;
        for(const clang::Decl* member = decl;;) {
            auto* tag = llvm::dyn_cast<clang::TagDecl>(member);
            if(tag && !tag->getIdentifier() && !tag->getTypedefNameForAnonDecl()) {
                return nameable = false;
            }
            const auto* context = member->getDeclContext();
            if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
               record && !accessible(member->getAccess(), record)) {
                return nameable = false;
            }
            if(context->isFunctionOrMethod()) {
                return nameable = context->Encloses(from);
            }
            if(context->isTranslationUnit()) {
                roots.emplace_back(root->getName(), entity_of(root));
                return true;
            }
            member = llvm::cast<clang::Decl>(context);
            auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(member);
            if(auto* named = llvm::dyn_cast<clang::NamedDecl>(member);
               named && !(ns && (ns->isAnonymousNamespace() || ns->isInline()))) {
                root = named;
            }
        }
    }

    bool accessible(clang::AccessSpecifier access, const clang::CXXRecordDecl* record) {
        if(access == clang::AS_public || access == clang::AS_none) {
            return true;
        }
        for(const auto* scope = from; scope; scope = scope->getParent()) {
            auto* enclosing = llvm::dyn_cast<clang::CXXRecordDecl>(scope);
            if(enclosing && (enclosing->getCanonicalDecl() == record->getCanonicalDecl() ||
                             (access == clang::AS_protected && enclosing->isDerivedFrom(record)))) {
                return true;
            }
        }
        return false;
    }

    const clang::DeclContext* from;
};

/// `std::name`, else `name`, when either declares `type` before `from`
/// opens, and so before anything inserted into it.
std::optional<std::string> standard_name(clang::ASTContext& context,
                                         clang::QualType type,
                                         llvm::StringRef name,
                                         const clang::DeclContext* from) {
    auto member = declaration_name(context, name);
    if(member.isEmpty()) {
        return std::nullopt;
    }
    auto opens = llvm::cast<clang::Decl>(from)->getBeginLoc();
    auto declares = [&](const Entities& found) {
        auto* alias =
            found.size() == 1 ? llvm::dyn_cast<clang::TypedefNameDecl>(found[0]) : nullptr;
        return alias && context.hasSameType(alias->getUnderlyingType(), type) &&
               (opens.isInvalid() ||
                context.getSourceManager().isBeforeInTranslationUnit(alias->getLocation(), opens));
    };
    if(auto std_name = declaration_name(context, "std"); !std_name.isEmpty()) {
        auto std_scope = lookup_qualified(context.getTranslationUnitDecl(), std_name);
        auto* ns =
            std_scope.size() == 1 ? llvm::dyn_cast<clang::NamespaceDecl>(std_scope[0]) : nullptr;
        if(ns && lookup_unqualified(from, std_name) == std_scope &&
           declares(lookup_qualified(ns, member))) {
            return ("std::" + name).str();
        }
    }
    if(declares(lookup_unqualified(from, member))) {
        return name.str();
    }
    return std::nullopt;
}

bool is_identifier_char(char c) {
    return llvm::isAlnum(c) || c == '_';
}

/// Respell the names of a type printed fully qualified for `from`. A name
/// drops the namespaces enclosing `from` when the shortened name still
/// finds the same entity there, and is anchored at the global scope when
/// even its full spelling would find another. The compiler's internal
/// names become their standard spellings; nullopt when `from` sees none.
std::optional<std::string> respell(clang::ASTContext& context,
                                   llvm::StringRef printed,
                                   const SpelledNames& names,
                                   const clang::DeclContext* from) {
    llvm::SmallVector<const clang::NamespaceDecl*, 4> namespaces;
    for(const auto* scope = from; scope; scope = scope->getParent()) {
        auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope);
        if(ns && !ns->isAnonymousNamespace() && !ns->isInline()) {
            namespaces.push_back(ns);
        }
    }
    std::ranges::reverse(namespaces);

    auto spell = [&](llvm::ArrayRef<llvm::StringRef> components) -> std::optional<std::string> {
        auto joined = [&](std::size_t skipped) {
            return llvm::join(components.drop_front(skipped), "::");
        };
        if(components.size() == 1) {
            for(const auto* sugar: names.predefined) {
                if(components[0] != sugar->getIdentifier()->getName()) {
                    continue;
                }
                switch(sugar->getKind()) {
                    case clang::PredefinedSugarType::Kind::SizeT:
                        return standard_name(context, clang::QualType(sugar, 0), "size_t", from);
                    case clang::PredefinedSugarType::Kind::PtrdiffT:
                        return standard_name(context, clang::QualType(sugar, 0), "ptrdiff_t", from);
                    case clang::PredefinedSugarType::Kind::SignedSizeT: return std::nullopt;
                }
            }
        }
        if(names.null_pointer && components.size() == 2 && components[0] == "std" &&
           components[1] == "nullptr_t") {
            return standard_name(context, context.NullPtrTy, "nullptr_t", from)
                .value_or("decltype(nullptr)");
        }
        const auto* root = llvm::find_if(names.roots, [&](const auto& entry) {
            return entry.first == components[0];
        });
        if(root == names.roots.end()) {
            return joined(0);
        }
        for(auto depth = std::min(namespaces.size(), components.size() - 1); depth > 0;
            depth -= 1) {
            auto prefix = llvm::ArrayRef(namespaces).take_front(depth);
            if(!llvm::equal(prefix, components.take_front(depth), [](auto* ns, auto component) {
                   return ns->getName() == component;
               })) {
                continue;
            }
            auto name = declaration_name(context, components[depth]);
            auto expected = lookup_qualified(prefix.back(), name);
            if(!expected.empty() && lookup_unqualified(from, name) == expected) {
                return joined(depth);
            }
        }
        auto found = lookup_unqualified(from, declaration_name(context, components[0]));
        if(found.size() == 1 && found[0] == root->second) {
            return joined(0);
        }
        return "::" + joined(0);
    };

    std::string result;
    llvm::StringRef rest = printed;
    while(!rest.empty()) {
        if(llvm::isDigit(rest.front()) || !is_identifier_char(rest.front())) {
            auto token = llvm::isDigit(rest.front()) ? rest.take_while(is_identifier_char)
                                                     : rest.take_front();
            result += token;
            rest = rest.drop_front(token.size());
            continue;
        }
        // A component after `::`, `.` or `->` continues a name or an
        // expression already respelled.
        llvm::StringRef done = result;
        if(done.ends_with("::") || done.ends_with(".") || done.ends_with("->")) {
            auto component = rest.take_while(is_identifier_char);
            result += component;
            rest = rest.drop_front(component.size());
            continue;
        }
        llvm::SmallVector<llvm::StringRef, 4> components;
        while(true) {
            components.push_back(rest.take_while(is_identifier_char));
            rest = rest.drop_front(components.back().size());
            if(!rest.starts_with("::") || rest.size() < 3 || !is_identifier_char(rest[2]) ||
               llvm::isDigit(rest[2])) {
                break;
            }
            rest = rest.drop_front(2);
        }
        auto spelled = spell(components);
        if(!spelled) {
            return std::nullopt;
        }
        result += *spelled;
    }
    return result;
}

/// One "template <...>" head, the parameters spelled without defaults,
/// followed by the requires-clause when the list has one.
std::string template_head(CompilationUnitRef unit,
                          const clang::TemplateParameterList* params,
                          const clang::DeclContext* from) {
    std::string head;
    llvm::raw_string_ostream os(head);
    os << "template <";
    for(auto [index, param]: llvm::enumerate(*params)) {
        if(index) {
            os << ", ";
        }
        if(auto* value = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param)) {
            os << type_name(unit.context(), value->getType(), from).value_or("auto");
            if(value->isParameterPack()) {
                os << "...";
            }
        } else {
            os << display::template_param_type(param).text;
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

/// Whether members appended at the end of the class body are public.
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

llvm::StringRef line_indent(llvm::StringRef content, std::uint32_t offset) {
    auto line = content.substr(line_begin(content, offset));
    return line.take_while([](char c) { return c == ' ' || c == '\t'; });
}

bool at_file_scope(const clang::DeclContext* context) {
    return context->isFileContext() ||
           llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context);
}

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

std::optional<std::string> type_name(clang::ASTContext& context,
                                     clang::QualType type,
                                     const clang::DeclContext* from,
                                     llvm::StringRef name) {
    // A deduced type keeps its `auto` as sugar, one layer per deduction
    // chained through, and a decltype names its type by an expression
    // `from` may not see: print the type beneath.
    while(true) {
        const auto* sugar = type.getTypePtr();
        clang::QualType beneath;
        if(auto* deduced = llvm::dyn_cast<clang::DeducedType>(sugar);
           deduced && deduced->isSugared()) {
            beneath = deduced->getDeducedType();
        } else if(auto* decltype_type = llvm::dyn_cast<clang::DecltypeType>(sugar);
                  decltype_type && decltype_type->isSugared()) {
            beneath = decltype_type->getUnderlyingType();
        } else {
            break;
        }
        type = context.getQualifiedType(beneath, type.getLocalQualifiers());
    }

    SpelledNames names(from);
    names.TraverseType(type);
    if(!names.nameable) {
        return std::nullopt;
    }

    clang::PrintingPolicy policy = context.getPrintingPolicy();
    policy.SuppressScope = false;
    policy.SuppressUnwrittenScope = true;
    policy.FullyQualifiedName = true;
    // The declarator name is printed as `@`, which no respelled name
    // contains, and put in last.
    std::string printed;
    llvm::raw_string_ostream os(printed);
    type.print(os, policy, name.empty() ? "" : "@");
    auto spelled = respell(context, printed, names, from);
    if(!spelled) {
        return std::nullopt;
    }
    if(!name.empty()) {
        spelled->replace(spelled->find('@'), 1, name);
    }
    return bind_declarators(std::move(*spelled));
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
        heads += template_head(unit, params, from);
        heads += '\n';
    }
    return heads;
}

std::optional<TextReplacement> insert_members(CompilationUnitRef unit,
                                              const clang::CXXRecordDecl* record,
                                              llvm::ArrayRef<std::string> lines) {
    auto brace = main_range(unit, record->getBraceRange().getEnd());
    auto head = main_range(unit, record->getBeginLoc());
    if(!brace || !head) {
        return std::nullopt;
    }
    auto content = unit.main_content();
    auto record_indent = line_indent(content, head->begin);
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
        indent = record_indent.str() + "    ";
    }

    std::string text;
    if(!own_line) {
        text += '\n';
    }
    if(!ends_public(record)) {
        text += record_indent;
        text += "public:\n";
    }
    for(const auto& line: lines) {
        text += indent;
        text += line;
        text += '\n';
    }
    if(!own_line) {
        text += record_indent;
    }
    auto offset = own_line ? begin : brace->begin;
    return TextReplacement{
        {offset, offset},
        std::move(text)};
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

auto code_actions(CompilationUnitRef unit, LocalSourceRange selection) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;
    action::add_include(unit, selection, out);

    auto path = unit.file_path(unit.main_file());
    bool main_is_header = is_header_path(path) || is_context_header_path(path);
    SelectionTree::create_each(unit, selection, [&](SelectionTree tree) {
        auto before = out.size();
        action::enumerate(unit, tree, main_is_header, out);
        return out.size() > before;
    });

    auto before = out.size();
    action::expand_macro(unit, selection, out);
    action::format_actions(unit, out, before);
    return out;
}

auto assemble_definitions(llvm::ArrayRef<DefinitionPiece> pieces,
                          llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::string> {
    std::string text;
    for(const auto& piece: pieces) {
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
