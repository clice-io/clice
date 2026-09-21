#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/display.h"

#include "llvm/ADT/STLExtras.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "clang/Basic/TokenKinds.h"

namespace clice::feature::action {

namespace {

/// Whether the declaration wants a definition this TU does not have.
bool definable(const clang::FunctionDecl* decl) {
    return !decl->isImplicit() && !decl->isInvalidDecl() && !decl->isThisDeclarationADefinition() &&
           !decl->isDefined() && !decl->isPureVirtual() &&
           decl->getFriendObjectKind() == clang::Decl::FOK_None &&
           decl->getTemplateSpecializationKind() != clang::TSK_ExplicitSpecialization &&
           !llvm::isa<clang::CXXDeductionGuideDecl>(decl);
}

/// Whether a definition can name the function from file scope: a member
/// of a local or unnamed class cannot be defined out of line.
bool qualifiable(const clang::FunctionDecl* decl) {
    for(const auto* context = decl->getDeclContext(); !context->isFileContext();
        context = context->getParent()) {
        auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
        if(!record || record->isLocalClass() || record->getName().empty()) {
            return false;
        }
    }
    return true;
}

/// Whether the definition may live in a source file: templates and
/// inline functions must stay visible to every includer.
bool host_definable(const clang::FunctionDecl* decl) {
    return !decl->isTemplated() && !decl->isInlineSpecified() && !decl->isConstexpr() &&
           !decl->isConsteval();
}

/// The name a definition spells: a constructor or destructor is named
/// after its class, not after the type its declaration name carries.
std::string function_name(const clang::FunctionDecl* decl) {
    if(auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(decl)) {
        return ctor->getParent()->getNameAsString();
    }
    if(auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(decl)) {
        return "~" + dtor->getParent()->getNameAsString();
    }
    return display::name_of(decl, {.qualified = false});
}

/// The declaration's continuation lines carry the indentation of its
/// original context; drop the first line's indentation from them.
std::string reindent(llvm::StringRef text, llvm::StringRef indent) {
    std::string out;
    bool first = true;
    for(llvm::StringRef line: llvm::split(text, '\n')) {
        if(!first) {
            out += '\n';
            line.consume_front(indent);
        }
        first = false;
        out += line;
    }
    return out;
}

/// A replacement inside the declaration's text, offsets relative to it.
struct Patch {
    std::uint32_t begin;
    std::uint32_t end;
    std::string text;
};

/// The declaration's source with the patches applied; nullopt when
/// patches overlap (a transform bug, the action is not offered).
std::optional<std::string> apply(llvm::StringRef text, std::vector<Patch> patches) {
    std::ranges::sort(patches, {}, [](const Patch& patch) {
        return std::pair(patch.begin, patch.end);
    });
    std::string result;
    std::uint32_t cursor = 0;
    for(const auto& patch: patches) {
        if(patch.begin < cursor) {
            return std::nullopt;
        }
        result += text.substr(cursor, patch.begin - cursor);
        result += patch.text;
        cursor = patch.end;
    }
    result += text.substr(cursor);
    return result;
}

clang::SourceRange default_argument_range(const clang::NamedDecl* param) {
    if(auto* type = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param)) {
        if(type->hasDefaultArgument() && !type->defaultArgumentWasInherited()) {
            return type->getDefaultArgument().getSourceRange();
        }
    } else if(auto* value = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param)) {
        if(value->hasDefaultArgument() && !value->defaultArgumentWasInherited()) {
            return value->getDefaultArgument().getSourceRange();
        }
    } else if(auto* tmpl = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param)) {
        if(tmpl->hasDefaultArgument() && !tmpl->defaultArgumentWasInherited()) {
            return tmpl->getDefaultArgument().getSourceRange();
        }
    }
    return {};
}

/// The source transform turning a declaration into the head of its
/// out-of-line definition, spelled for insertion into `from`.
class Transform {
public:
    Transform(CompilationUnitRef unit,
              const clang::FunctionDecl* decl,
              const clang::DeclContext* from) : unit(unit), decl(decl), from(from) {}

    std::optional<std::string> run() {
        auto range = written_declaration(decl)->getSourceRange();
        auto text = spelled_text(unit, range);
        if(!text) {
            return std::nullopt;
        }
        fid = unit.file_id(range.getBegin());
        base = unit.file_offset(range.getBegin());
        source = *text;
        tokens = unit.spelled_tokens(range);

        auto name = offset_of(decl->getNameInfo().getBeginLoc());
        if(!name) {
            return std::nullopt;
        }
        drop_specifiers(*name);
        drop_override_attributes();
        if(!drop_default_arguments() || !qualify_name(*name)) {
            return std::nullopt;
        }
        qualify_return_type();

        auto head = apply(source, std::move(patches));
        if(!head) {
            return std::nullopt;
        }
        auto indent = line_indent(unit.file_content(fid), base);
        return template_heads(unit, decl, from) + reindent(*head, indent) + " {\n}\n";
    }

private:
    std::optional<std::uint32_t> offset_of(clang::SourceLocation location) {
        if(!location.isFileID() || unit.file_id(location) != fid) {
            return std::nullopt;
        }
        return unit.file_offset(location) - base;
    }

    /// The end of a token at `offset`, with the spaces following it.
    std::uint32_t past_spaces(std::uint32_t offset) {
        while(offset < source.size() && source[offset] == ' ') {
            offset += 1;
        }
        return offset;
    }

    std::uint32_t before_spaces(std::uint32_t offset) {
        while(offset > 0 && source[offset - 1] == ' ') {
            offset -= 1;
        }
        return offset;
    }

    /// `virtual`, `static` and `explicit` are declaration-only.
    void drop_specifiers(std::uint32_t name) {
        for(const auto& token: tokens) {
            auto offset = offset_of(token.location());
            if(!offset || *offset >= name) {
                break;
            }
            if(token.kind() == clang::tok::kw_virtual || token.kind() == clang::tok::kw_static ||
               token.kind() == clang::tok::kw_explicit) {
                patches.push_back({*offset, past_spaces(*offset + token.length()), ""});
            }
        }
    }

    void drop_override_attributes() {
        for(const auto* attr: decl->attrs()) {
            if(!llvm::isa<clang::OverrideAttr, clang::FinalAttr>(attr)) {
                continue;
            }
            if(auto offset = offset_of(attr->getLocation())) {
                patches.push_back(
                    {before_spaces(*offset), *offset + unit.token_length(attr->getLocation()), ""});
            }
        }
    }

    /// Delete `= expr` for a default argument spanning `range`.
    bool drop_default(clang::SourceRange range) {
        auto begin = offset_of(range.getBegin());
        auto end = offset_of(range.getEnd());
        if(!begin || !end) {
            return false;
        }
        std::optional<std::uint32_t> equal;
        for(const auto& token: tokens) {
            auto offset = offset_of(token.location());
            if(!offset || *offset >= *begin) {
                break;
            }
            equal = token.kind() == clang::tok::equal ? offset : std::nullopt;
        }
        if(!equal) {
            return false;
        }
        patches.push_back({before_spaces(*equal), *end + unit.token_length(range.getEnd()), ""});
        return true;
    }

    /// Default arguments belong to the declaration alone: the function's
    /// parameters and, for a function template, its own template
    /// parameters.
    bool drop_default_arguments() {
        for(const auto* param: decl->parameters()) {
            if(auto range = param->getDefaultArgRange(); range.isValid() && !drop_default(range)) {
                return false;
            }
        }
        if(auto* described = decl->getDescribedFunctionTemplate()) {
            for(const auto* param: *described->getTemplateParameters()) {
                if(auto range = default_argument_range(param);
                   range.isValid() && !drop_default(range)) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Prefix the name with the qualifier `from` needs, replacing one the
    /// declaration already spells.
    bool qualify_name(std::uint32_t name) {
        auto qualifier = qualifier_at(decl->getDeclContext(), from);
        if(auto written = decl->getQualifierLoc()) {
            auto begin = offset_of(written.getBeginLoc());
            if(!begin) {
                return false;
            }
            patches.push_back({*begin, name, qualifier});
            return true;
        }
        if(qualifier.empty()) {
            return true;
        }
        // A destructor's name begins at its `~`.
        patches.push_back({name, name, qualifier});
        return true;
    }

    /// The leading return type is looked up at the definition's scope,
    /// unlike the parameters, which the qualified name puts in the
    /// class's scope: spell it fully qualified. Deduced and dependent
    /// return types stay as written.
    void qualify_return_type() {
        auto range = decl->getReturnTypeSourceRange();
        auto type = decl->getReturnType();
        if(range.isInvalid() || type->isDependentType() || type->getContainedAutoType() ||
           llvm::isa<clang::DecltypeType>(type)) {
            return;
        }
        auto begin = offset_of(range.getBegin());
        auto end = offset_of(range.getEnd());
        if(!begin || !end) {
            return;
        }
        *end += unit.token_length(range.getEnd());
        // cv-qualifiers have no location of their own: fold the ones
        // spelled around the written type into the replaced span.
        auto is_cv = [](const clang::syntax::Token& token) {
            return token.kind() == clang::tok::kw_const || token.kind() == clang::tok::kw_volatile;
        };
        for(auto [index, token]: llvm::enumerate(tokens)) {
            auto offset = offset_of(token.location());
            if(!offset) {
                continue;
            }
            if(*offset + token.length() == before_spaces(*begin) && is_cv(token)) {
                *begin = *offset;
            }
            if(*offset == past_spaces(*end) && is_cv(token)) {
                *end = *offset + token.length();
            }
        }
        patches.push_back({*begin, *end, type_name(unit.context(), type, from)});
    }

    CompilationUnitRef unit;
    const clang::FunctionDecl* decl;
    const clang::DeclContext* from;

    clang::FileID fid;
    std::uint32_t base = 0;
    llvm::StringRef source;
    llvm::ArrayRef<clang::syntax::Token> tokens;
    std::vector<Patch> patches;
};

std::optional<std::string> definition_text(CompilationUnitRef unit,
                                           const clang::FunctionDecl* decl,
                                           const clang::DeclContext* from) {
    return Transform(unit, decl, from).run();
}

/// Where a same-file definition goes and the scope it is spelled for.
struct Placement {
    std::uint32_t offset;
    const clang::DeclContext* from;
    /// Code follows on the next line: the definition wants a blank line
    /// after it too.
    bool code_follows;
};

/// Past the declaration's terminating `;` (when it directly follows) and
/// the rest of that line.
std::uint32_t past_declaration(llvm::StringRef content, std::uint32_t end) {
    auto cursor = end;
    while(cursor < content.size() && (content[cursor] == ' ' || content[cursor] == '\t')) {
        cursor += 1;
    }
    if(cursor < content.size() && content[cursor] == ';') {
        cursor += 1;
    }
    return line_end(content, cursor - 1);
}

const clang::CXXRecordDecl* outermost_record(const clang::CXXRecordDecl* record) {
    while(auto* parent = llvm::dyn_cast<clang::CXXRecordDecl>(record->getDeclContext())) {
        record = parent;
    }
    return record;
}

/// After the last out-of-line definition of the class's members in the
/// main file, else after the (outermost enclosing) class itself.
std::optional<Placement> member_placement(CompilationUnitRef unit,
                                          const clang::CXXRecordDecl* record) {
    auto content = unit.main_content();
    auto placement = [&](std::uint32_t offset, const clang::DeclContext* from) {
        return Placement{
            .offset = offset,
            .from = from,
            .code_follows = offset < content.size() && content[offset] != '\n',
        };
    };
    auto definitions = out_of_line_definitions(unit, record);
    if(!definitions.empty()) {
        const auto* last = definitions.back();
        auto range = main_range(unit, written_declaration(last)->getSourceRange());
        if(!range) {
            return std::nullopt;
        }
        return placement(line_end(content, range->end - 1), last->getLexicalDeclContext());
    }
    const auto* outermost = outermost_record(record);
    auto range = main_range(unit, outermost->getSourceRange());
    if(!range) {
        return std::nullopt;
    }
    return placement(past_declaration(content, range->end), outermost->getLexicalDeclContext());
}

std::optional<Placement> placement_of(CompilationUnitRef unit, const clang::FunctionDecl* decl) {
    if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return member_placement(unit, method->getParent());
    }
    auto range = main_range(unit, written_declaration(decl)->getSourceRange());
    if(!range) {
        return std::nullopt;
    }
    auto content = unit.main_content();
    auto offset = past_declaration(content, range->end);
    return Placement{
        .offset = offset,
        .from = decl->getLexicalDeclContext(),
        .code_follows = offset < content.size() && content[offset] != '\n',
    };
}

std::uint64_t container_entity(CompilationUnitRef unit, const clang::FunctionDecl* decl) {
    if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return unit.entity(outermost_record(method->getParent()));
    }
    return 0;
}

CodeAction define_action(std::string id, std::string title, DefineRequest request) {
    return CodeAction{
        .id = std::move(id),
        .title = std::move(title),
        .kind = CodeActionKind::RefactorRewrite,
        .index = std::move(request),
    };
}

DefineRequest at_placement(const Placement& placement, std::vector<DefinitionPiece> pieces) {
    return DefineRequest{
        .range = {placement.offset, placement.offset},
        .before = "\n",
        .after = placement.code_follows ? "\n" : "",
        .pieces = std::move(pieces),
    };
}

}  // namespace

void define(const Context& ctx, std::vector<CodeAction>& out) {
    const auto* decl = ctx.node.get<clang::FunctionDecl>();
    if(!decl || !definable(decl)) {
        return;
    }
    auto unit = ctx.unit;
    auto entity = unit.entity(decl);
    auto name = function_name(decl);
    auto qualified = [&](const clang::DeclContext* from) {
        return qualifier_at(decl->getDeclContext(), from) + name;
    };

    if(llvm::isa<clang::CXXMethodDecl>(decl)) {
        auto range = main_range(unit, written_declaration(decl)->getSourceRange());
        if(range) {
            auto content = unit.main_content();
            auto semicolon = range->end;
            while(semicolon < content.size() && content[semicolon] == ' ') {
                semicolon += 1;
            }
            if(semicolon < content.size() && content[semicolon] == ';') {
                out.push_back(define_action("define-in-class",
                                            std::format("Define '{}' inline", name),
                                            DefineRequest{
                                                .range = {semicolon, semicolon + 1},
                                                .before = " ",
                                                .pieces = {{entity, "{}"}},
                }));
            }
        }
    }

    if(!qualifiable(decl)) {
        return;
    }
    if(auto placement = placement_of(unit, decl)) {
        if(auto text = definition_text(unit, decl, placement->from)) {
            out.push_back(
                define_action("define-out-of-line",
                              std::format("Define '{}' out of line", qualified(placement->from)),
                              at_placement(*placement,
                                           {
                                               {entity, std::move(*text)}
            })));
        }
    }
    if(ctx.main_is_header && host_definable(decl)) {
        if(auto text = definition_text(unit, decl, unit.tu())) {
            out.push_back(define_action("define-in-host",
                                        std::format("Define '{}'", qualified(unit.tu())),
                                        DefineRequest{
                                            .host = true,
                                            .container = container_entity(unit, decl),
                                            .pieces = {{entity, std::move(*text)}},
                                        }));
        }
    }
}

void define_missing(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record) {
        const auto* method = ctx.node.get<clang::CXXMethodDecl>();
        if(!method || !method->isThisDeclarationADefinition() || !method->isOutOfLine() ||
           !method->getLexicalDeclContext()->isFileContext()) {
            return;
        }
        record = method->getParent();
    }
    record = record->getDefinition();
    if(!record || record->isLocalClass() || record->getName().empty()) {
        return;
    }

    std::vector<const clang::FunctionDecl*> missing;
    for(const auto* member: record->decls()) {
        if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(member)) {
            member = described->getTemplatedDecl();
        }
        auto* function = llvm::dyn_cast<clang::FunctionDecl>(member);
        if(function && definable(function) && qualifiable(function)) {
            missing.push_back(function);
        }
    }
    if(missing.empty()) {
        return;
    }

    auto pieces = [&](const clang::DeclContext* from, bool host) {
        std::vector<DefinitionPiece> pieces;
        for(const auto* function: missing) {
            if(host && !host_definable(function)) {
                continue;
            }
            if(auto text = definition_text(unit, function, from)) {
                pieces.push_back({unit.entity(function), std::move(*text)});
            }
        }
        return pieces;
    };

    auto name = display::name_of(record, {.qualified = false});
    if(auto placement = member_placement(unit, record)) {
        if(auto same_file = pieces(placement->from, false); !same_file.empty()) {
            out.push_back(define_action("define-missing",
                                        std::format("Define missing members of '{}'", name),
                                        at_placement(*placement, std::move(same_file))));
        }
    }
    if(ctx.main_is_header) {
        if(auto host = pieces(unit.tu(), true); !host.empty()) {
            out.push_back(define_action("define-missing-in-host",
                                        std::format("Define missing members of '{}'", name),
                                        DefineRequest{
                                            .host = true,
                                            .container = unit.entity(outermost_record(record)),
                                            .pieces = std::move(host),
                                        }));
        }
    }
}

}  // namespace clice::feature::action
