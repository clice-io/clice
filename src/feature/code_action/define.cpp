#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <utility>
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
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice::feature::action {

namespace {

/// `void f(), g();` declares two functions in one declaration: the text
/// belongs to both, so neither can be rewritten on its own.
bool shares_declaration(const clang::FunctionDecl* decl) {
    return llvm::any_of(decl->getDeclContext()->decls(), [&](const clang::Decl* sibling) {
        return sibling != decl && !sibling->isImplicit() &&
               sibling->getBeginLoc() == decl->getBeginLoc();
    });
}

/// Whether the declaration wants a definition this TU does not have. A
/// function declared through a typedef of its type (`Handler on_event;`)
/// spells no parameter list a definition could reuse.
bool definable(const clang::FunctionDecl* decl) {
    return !decl->isImplicit() && !decl->isInvalidDecl() && !decl->isThisDeclarationADefinition() &&
           !decl->isDefined() && !decl->isPureVirtual() &&
           decl->getFriendObjectKind() == clang::Decl::FOK_None &&
           decl->getTemplateSpecializationKind() != clang::TSK_ExplicitSpecialization &&
           !llvm::isa<clang::CXXDeductionGuideDecl>(decl) && decl->getFunctionTypeLoc() &&
           !shares_declaration(decl);
}

/// Whether a definition can name the function from file scope: a member
/// of a local or unnamed class cannot be defined out of line.
bool qualifiable(const clang::FunctionDecl* decl) {
    for(const auto* context = decl->getDeclContext(); !at_file_scope(context);
        context = context->getParent()) {
        auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
        if(!record || record->isLocalClass() || record->getName().empty()) {
            return false;
        }
    }
    return true;
}

/// Whether the program holds one definition of the function, which then
/// belongs in a source file: templates and inline functions are defined
/// in every TU that uses them, a function without external linkage in
/// every TU that declares it. Written in a header, that one definition
/// needs `inline`.
bool defined_once(const clang::FunctionDecl* decl) {
    return !decl->isTemplated() && !decl->isInlineSpecified() && !decl->isConstexpr() &&
           !decl->isConsteval() && decl->isExternallyVisible();
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

/// The declaration's source with the patches applied. The patches touch
/// disjoint parts of the declaration by construction: specifiers outside
/// the return type, which absorbs the ones among its tokens, the return
/// type before the name, defaults inside the parameters, `override` past
/// them. Insertions at one offset keep the order they were made in.
std::string apply(llvm::StringRef text, std::vector<Patch> patches) {
    std::ranges::stable_sort(patches, {}, [](const Patch& patch) {
        return std::pair(patch.begin, patch.end);
    });
    std::string result;
    std::uint32_t cursor = 0;
    for(const auto& patch: patches) {
        assert(patch.begin >= cursor && "overlapping patches");
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

bool is_cv(const clang::syntax::Token& token) {
    return token.kind() == clang::tok::kw_const || token.kind() == clang::tok::kw_volatile;
}

/// The keywords a function's decl-specifiers may mix with its return type.
bool is_specifier(const clang::syntax::Token& token) {
    switch(token.kind()) {
        case clang::tok::kw_static:
        case clang::tok::kw_virtual:
        case clang::tok::kw_inline:
        case clang::tok::kw_constexpr:
        case clang::tok::kw_consteval:
        case clang::tok::kw_extern: return true;
        default: return false;
    }
}

/// `virtual`, `static` and `explicit` are declaration-only.
bool declaration_only(const clang::syntax::Token& token) {
    return token.kind() == clang::tok::kw_virtual || token.kind() == clang::tok::kw_static ||
           token.kind() == clang::tok::kw_explicit;
}

/// Where the type the decl-specifiers spell is written beneath a return
/// type's declarator (`const R` of `const R* (*f())(int)`).
clang::TypeLoc specifier_loc(clang::TypeLoc loc) {
    while(true) {
        auto unqualified = loc.getUnqualifiedLoc();
        if(auto pointer = unqualified.getAs<clang::PointerTypeLoc>()) {
            loc = pointer.getPointeeLoc();
        } else if(auto reference = unqualified.getAs<clang::ReferenceTypeLoc>()) {
            loc = reference.getPointeeLoc();
        } else if(auto paren = unqualified.getAs<clang::ParenTypeLoc>()) {
            loc = paren.getInnerLoc();
        } else if(auto function = unqualified.getAs<clang::FunctionTypeLoc>()) {
            loc = function.getReturnLoc();
        } else if(auto array = unqualified.getAs<clang::ArrayTypeLoc>()) {
            loc = array.getElementLoc();
        } else {
            return loc;
        }
    }
}

/// How a declaration turns into the head of its out-of-line definition.
struct DefinitionOptions {
    /// Mark the definition `inline`: it stays in a header.
    bool mark_inline = false;
};

/// The source transform turning a declaration into the head of its
/// out-of-line definition, spelled for insertion into `from`.
class Transform {
public:
    Transform(CompilationUnitRef unit,
              const clang::FunctionDecl* decl,
              const clang::DeclContext* from,
              DefinitionOptions options) : unit(unit), decl(decl), from(from), options(options) {}

    std::optional<std::string> run() {
        auto range = written_declaration(decl)->getSourceRange();
        auto text = spelled_text(unit, range);
        if(!text) {
            return std::nullopt;
        }
        fid = unit.file_id(range.getBegin());
        base = unit.file_offset(range.getBegin());
        source = *text;
        // Lexed here rather than taken from the token buffer, which holds
        // none for a header the preamble compiled.
        tokens = clang::syntax::tokenize(clang::syntax::FileRange(fid, base, base + source.size()),
                                         unit.context().getSourceManager(),
                                         unit.lang_options());

        auto name = offset_of(decl->getNameInfo().getBeginLoc());
        if(!name) {
            return std::nullopt;
        }
        if(options.mark_inline && !add_inline()) {
            return std::nullopt;
        }
        qualify_return_type(*name);
        drop_override_attributes();
        if(!drop_specifiers(*name) || !drop_default_arguments() || !qualify_name(*name)) {
            return std::nullopt;
        }

        auto indent = line_indent(unit.file_content(fid), base);
        return template_heads(unit, decl, from) +
               reindent(apply(source, std::move(patches)), indent) + " {\n}\n";
    }

private:
    /// A location of the declaration relative to its text; nullopt when
    /// a macro spells it.
    std::optional<std::uint32_t> offset_of(clang::SourceLocation location) {
        if(!location.isFileID() || unit.file_id(location) != fid) {
            return std::nullopt;
        }
        return unit.file_offset(location) - base;
    }

    /// The tokens all lie in the declaration's text.
    std::uint32_t offset_of(const clang::syntax::Token& token) {
        return unit.file_offset(token.location()) - base;
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

    /// The index of the token closing the bracket opened at `open`, a `(`
    /// or a `[`; nullopt when a macro spells it.
    std::optional<std::size_t> closing(std::size_t open) {
        auto kind = tokens[open].kind();
        auto close = kind == clang::tok::l_paren ? clang::tok::r_paren : clang::tok::r_square;
        std::uint32_t depth = 0;
        for(auto index = open; index < tokens.size(); index += 1) {
            if(tokens[index].kind() == kind) {
                depth += 1;
            } else if(tokens[index].kind() == close) {
                depth -= 1;
                if(depth == 0) {
                    return index;
                }
            }
        }
        return std::nullopt;
    }

    /// `inline` joins the decl-specifiers, which only the declaration's
    /// `[[...]]` attributes may precede.
    bool add_inline() {
        std::size_t index = 0;
        while(tokens[index].kind() == clang::tok::l_square &&
              tokens[index + 1].kind() == clang::tok::l_square) {
            auto close = closing(index);
            if(!close) {
                return false;
            }
            index = *close + 1;
        }
        auto offset = offset_of(tokens[index]);
        patches.push_back({offset, offset, "inline "});
        return true;
    }

    /// `virtual`, `static` and `explicit`, with the condition of an
    /// `explicit(...)`, outside the return type, which drops its own.
    bool drop_specifiers(std::uint32_t name) {
        for(std::size_t index = 0; index < tokens.size(); index += 1) {
            auto offset = offset_of(tokens[index]);
            if(offset >= name) {
                break;
            }
            if(!declaration_only(tokens[index]) ||
               (return_type && offset >= return_type->first && offset < return_type->second)) {
                continue;
            }
            auto last = index;
            if(tokens[index].kind() == clang::tok::kw_explicit &&
               tokens[index + 1].kind() == clang::tok::l_paren) {
                auto close = closing(index + 1);
                if(!close) {
                    return false;
                }
                last = *close;
            }
            patches.push_back(
                {offset, past_spaces(offset_of(tokens[last]) + tokens[last].length()), ""});
            index = last;
        }
        return true;
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
            auto offset = offset_of(token);
            if(offset >= *begin) {
                break;
            }
            equal = token.kind() == clang::tok::equal ? std::optional(offset) : std::nullopt;
        }
        if(!equal) {
            return false;
        }
        patches.push_back({before_spaces(*equal), *end + unit.token_length(range.getEnd()), ""});
        return true;
    }

    /// Default arguments belong to the declaration alone: the function's
    /// parameters and, for a function template, its own template
    /// parameters. An inherited default is spelled by an earlier
    /// declaration, not this one.
    bool drop_default_arguments() {
        for(const auto* param: decl->parameters()) {
            if(param->hasInheritedDefaultArg()) {
                continue;
            }
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
    /// declaration already spells. A destructor's name begins at its `~`.
    bool qualify_name(std::uint32_t name) {
        auto qualifier = qualifier_at(decl->getDeclContext(), from);
        if(auto written = decl->getQualifierLoc()) {
            auto begin = offset_of(written.getBeginLoc());
            if(!begin) {
                return false;
            }
            patches.push_back({*begin, name, qualifier});
        } else if(!qualifier.empty()) {
            patches.push_back({name, name, qualifier});
        }
        return true;
    }

    /// The spelling at `from` of the type the decl-specifiers name; nullopt
    /// for a deduced or a dependent type, which stays as written. A class
    /// template enclosing the function, or a type one declares, is named
    /// through the template's parameters (`typename S<T>::size_type`).
    std::optional<std::string> specifier_spelling(clang::QualType type) {
        if(type->getContainedAutoType() || llvm::isa<clang::DecltypeType>(type)) {
            return std::nullopt;
        }
        const clang::TypeDecl* named = nullptr;
        if(auto* alias = llvm::dyn_cast<clang::TypedefType>(type.getTypePtr())) {
            named = alias->getDecl();
        } else if(auto* tag = llvm::dyn_cast<clang::TagType>(type.getTypePtr())) {
            named = tag->getDecl();
        }
        auto encloses = [&](const clang::DeclContext* context) {
            auto* record = llvm::dyn_cast_if_present<clang::CXXRecordDecl>(context);
            return record && record->isDependentContext() &&
                   record->Encloses(decl->getDeclContext());
        };
        std::string spelled;
        if(auto* record = llvm::dyn_cast_if_present<clang::CXXRecordDecl>(named);
           encloses(record)) {
            spelled = qualifier_at(record, from);
            spelled.resize(spelled.size() - 2);
        } else if(named && encloses(named->getDeclContext())) {
            spelled = qualifier_at(named->getDeclContext(), from) + named->getName().str();
        } else if(type->isDependentType()) {
            return std::nullopt;
        } else {
            return type_name(unit.context(), type, from);
        }
        if(llvm::isa<clang::CXXRecordDecl>(named->getDeclContext())) {
            spelled = "typename " + spelled;
        }
        auto qualifiers = type.getLocalQualifiers();
        return qualifiers.empty() ? spelled : qualifiers.getAsString() + " " + spelled;
    }

    /// The decl-specifiers are looked up at the definition's scope, unlike
    /// the parameters and the rest of the declarator, which follow the
    /// qualified name into the class's scope: spell the type they name for
    /// `from`. The keywords written among its tokens (`unsigned static
    /// long`) fold into the replaced span, the declaration-only ones
    /// dropped.
    void qualify_return_type(std::uint32_t name) {
        auto loc = specifier_loc(decl->getFunctionTypeLoc().getReturnLoc());
        auto type = loc.getType();
        auto begin = offset_of(loc.getBeginLoc());
        auto end = offset_of(loc.getEndLoc());
        // A constructor's return type has no location; a conversion
        // function spells its type inside its name.
        if(!begin || !end || *end >= name) {
            return;
        }
        auto spelling = specifier_spelling(type);
        if(!spelling) {
            return;
        }
        auto first = std::ranges::find_if(tokens, [&](const clang::syntax::Token& token) {
            return offset_of(token) >= *begin;
        });
        // The end may fall inside a token: the `>` of `A<B<int>>` is half
        // of a `>>`.
        auto last = std::prev(std::ranges::find_if(tokens, [&](const clang::syntax::Token& token) {
            return offset_of(token) > *end;
        }));
        auto absorbed = [](const clang::syntax::Token& token) {
            return is_cv(token) || is_specifier(token);
        };
        while(first != tokens.begin() && absorbed(*std::prev(first))) {
            --first;
        }
        while(std::next(last) != tokens.end() && absorbed(*std::next(last))) {
            ++last;
        }
        std::string text;
        for(const auto& token: llvm::make_range(first, std::next(last))) {
            if(is_specifier(token) && !declaration_only(token)) {
                text += token.text(unit.context().getSourceManager());
                text += ' ';
            }
        }
        text += *spelling;
        return_type = {offset_of(*first), offset_of(*last) + last->length()};
        patches.push_back({return_type->first, return_type->second, std::move(text)});
    }

    CompilationUnitRef unit;
    const clang::FunctionDecl* decl;
    const clang::DeclContext* from;
    DefinitionOptions options;

    clang::FileID fid;
    std::uint32_t base = 0;
    llvm::StringRef source;
    std::vector<clang::syntax::Token> tokens;
    std::vector<Patch> patches;
    /// The span the respelled return type replaces.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> return_type;
};

std::optional<std::string> definition_text(CompilationUnitRef unit,
                                           const clang::FunctionDecl* decl,
                                           const clang::DeclContext* from,
                                           DefinitionOptions options = {}) {
    return Transform(unit, decl, from, options).run();
}

/// Where a same-file definition goes and the scope it is spelled for.
struct Placement {
    std::uint32_t offset;
    const clang::DeclContext* from;
    /// The end of the declaration the definition follows.
    clang::SourceLocation after;
    /// Code follows on the line the definition starts: it wants a blank
    /// line after it too.
    bool code_follows;
};

/// The declaration at file scope a definition of `decl` can follow: the
/// declaration itself, or the outermost one lexically enclosing it, a
/// class or the function a block-scope declaration is in.
const clang::Decl* file_scope_anchor(const clang::Decl* decl) {
    while(!at_file_scope(decl->getLexicalDeclContext())) {
        decl = clang::Decl::castFromDeclContext(decl->getLexicalDeclContext());
    }
    return decl;
}

/// The outermost class lexically enclosing `record`, the one whose end
/// completes the members' bodies written inline.
const clang::CXXRecordDecl* outermost_record(const clang::CXXRecordDecl* record) {
    while(auto* parent = llvm::dyn_cast<clang::CXXRecordDecl>(record->getLexicalDeclContext())) {
        record = parent;
    }
    return record;
}

/// The offset of the `;` directly following, past blanks, a declaration
/// ending at `end`.
std::optional<std::uint32_t> semicolon_after(llvm::StringRef content, std::uint32_t end) {
    while(end < content.size() && (content[end] == ' ' || content[end] == '\t')) {
        end += 1;
    }
    if(end < content.size() && content[end] == ';') {
        return end;
    }
    return std::nullopt;
}

/// After the declaration statement `anchor` is part of (`struct S {...}
/// s;` and `typedef struct S {...} T;` end past the name): past its `;`,
/// and past the rest of that line when only blanks or a comment remain
/// there; right after it otherwise, inside whatever encloses it on that
/// line (`namespace ns { void f(); }`).
std::optional<Placement> placement_after(CompilationUnitRef unit, const clang::Decl* anchor) {
    // A class still being typed has no closing brace, nor has the class of
    // a lambda, which at file scope sits in a variable's initializer.
    if(auto* tag = llvm::dyn_cast<clang::TagDecl>(anchor);
       tag && tag->getBraceRange().getEnd().isInvalid()) {
        return std::nullopt;
    }
    auto& sm = unit.context().getSourceManager();
    auto end = anchor->getEndLoc();
    for(auto* next = anchor->getNextDeclInContext(); next; next = next->getNextDeclInContext()) {
        // The global `operator new` and kin are declared on first use, in
        // the middle of the context's declarations.
        if(next->isImplicit()) {
            continue;
        }
        if(sm.isBeforeInTranslationUnit(anchor->getBeginLoc(), next->getBeginLoc())) {
            break;
        }
        end = next->getEndLoc();
    }
    auto range = main_range(unit, end);
    if(!range) {
        return std::nullopt;
    }
    auto content = unit.main_content();
    auto offset = range->end;
    if(auto semicolon = semicolon_after(content, offset)) {
        offset = *semicolon + 1;
    }
    auto line = line_end(content, offset);
    auto rest = content.slice(offset, line).trim();
    if(rest.empty() || rest.starts_with("//")) {
        offset = line;
    }
    return Placement{
        .offset = offset,
        .from = anchor->getLexicalDeclContext(),
        .after = end,
        .code_follows = offset < content.size() && content[offset] != '\n' &&
                        offset == line_begin(content, offset),
    };
}

/// After the last out-of-line definition of the class's members in the
/// main file, else after the class.
std::optional<Placement> member_placement(CompilationUnitRef unit,
                                          const clang::CXXRecordDecl* record) {
    auto definitions = out_of_line_definitions(unit, record);
    if(!definitions.empty()) {
        return placement_after(unit, written_declaration(definitions.back()));
    }
    return placement_after(unit, file_scope_anchor(record));
}

std::optional<Placement> placement_of(CompilationUnitRef unit, const clang::FunctionDecl* decl) {
    if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return member_placement(unit, method->getParent());
    }
    return placement_after(unit, file_scope_anchor(written_declaration(decl)));
}

/// The last definition of the classes a definition of the function needs
/// complete, its return and parameter types held by value; null when it
/// needs none, nullopt when one has no definition in this TU. A template
/// specialization is taken as complete: using it instantiates it. So is
/// a class the compiler defines itself, such as AArch64's `va_list`.
std::optional<const clang::TagDecl*> last_needed_definition(CompilationUnitRef unit,
                                                            const clang::FunctionDecl* decl) {
    auto& sm = unit.context().getSourceManager();
    const clang::TagDecl* last = nullptr;
    auto need = [&](clang::QualType type) {
        auto* record = type->isDependentType() ? nullptr : type->getAsCXXRecordDecl();
        if(!record || llvm::isa<clang::ClassTemplateSpecializationDecl>(record)) {
            return true;
        }
        auto* definition = record->getDefinition();
        if(!definition) {
            return false;
        }
        if(definition->getEndLoc().isValid() &&
           (!last || sm.isBeforeInTranslationUnit(last->getEndLoc(), definition->getEndLoc()))) {
            last = definition;
        }
        return true;
    };
    if(!need(decl->getReturnType())) {
        return std::nullopt;
    }
    for(const auto* param: decl->parameters()) {
        if(!need(param->getType())) {
            return std::nullopt;
        }
    }
    return last;
}

/// The placement moved past `definition` when it lies further on in the
/// main file; nullopt when the definition comes later elsewhere, or where
/// it ends the function's scope is out of reach.
std::optional<Placement> placement_past(CompilationUnitRef unit,
                                        Placement placement,
                                        const clang::TagDecl* definition,
                                        const clang::FunctionDecl* decl) {
    auto& sm = unit.context().getSourceManager();
    if(!sm.isBeforeInTranslationUnit(placement.after, definition->getEndLoc())) {
        return placement;
    }
    auto moved = placement_after(unit, file_scope_anchor(definition));
    if(!moved || !moved->from->getRedeclContext()->Encloses(decl->getDeclContext())) {
        return std::nullopt;
    }
    return moved;
}

std::uint64_t container_entity(CompilationUnitRef unit, const clang::FunctionDecl* decl) {
    if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return unit.entity(outermost_record(method->getParent()));
    }
    return 0;
}

CodeAction define_action(std::string title, IndexRequest request) {
    return CodeAction{
        .title = std::move(title),
        .kind = protocol::CodeActionKind::refactor_rewrite,
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
    auto needed = last_needed_definition(unit, decl);
    bool header_once = ctx.main_is_header && defined_once(decl);

    // A body written in the class is compiled at the end of the outermost
    // class, where the types it needs must be complete.
    auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
    auto& sm = unit.context().getSourceManager();
    if(method && needed &&
       (!*needed ||
        !sm.isBeforeInTranslationUnit(outermost_record(method->getParent())->getEndLoc(),
                                      (*needed)->getEndLoc()))) {
        auto range = main_range(unit, written_declaration(decl)->getSourceRange());
        if(auto semicolon =
               range ? semicolon_after(unit.main_content(), range->end) : std::nullopt) {
            out.push_back(define_action(std::format("Define '{}' inline", name),
                                        DefineRequest{
                                            .range = {*semicolon, *semicolon + 1},
                                            .before = " ",
                                            .pieces = {{entity, "{}"}},
            }));
        }
    }

    if(!qualifiable(decl)) {
        return;
    }
    auto placement = needed ? placement_of(unit, decl) : std::nullopt;
    if(placement && *needed) {
        placement = placement_past(unit, *placement, *needed, decl);
    }
    if(placement) {
        if(auto text = definition_text(unit, decl, placement->from, {.mark_inline = header_once})) {
            out.push_back(
                define_action(std::format("Define '{}' out of line", qualified(placement->from)),
                              at_placement(*placement,
                                           {
                                               {entity, std::move(*text)}
            })));
        }
    }
    if(header_once) {
        if(auto text = definition_text(unit, decl, unit.tu())) {
            out.push_back(define_action(std::format("Define '{}'", qualified(unit.tu())),
                                        DefineInHostRequest{
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
           !at_file_scope(method->getLexicalDeclContext())) {
            return;
        }
        record = method->getParent();
    }
    record = record->getDefinition();
    if(!record || record->isLocalClass() || record->getName().empty()) {
        return;
    }

    // The host source takes the missing members defined once; this file
    // those whose types are complete somewhere in it, all past the last of
    // those types.
    std::vector<const clang::FunctionDecl*> missing;
    std::vector<const clang::FunctionDecl*> placed;
    auto placement = member_placement(unit, record);
    for(const auto* member: record->decls()) {
        if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(member)) {
            member = described->getTemplatedDecl();
        }
        auto* function = llvm::dyn_cast<clang::FunctionDecl>(member);
        if(!function || !definable(function) || !qualifiable(function)) {
            continue;
        }
        missing.push_back(function);
        auto needed = last_needed_definition(unit, function);
        if(!placement || !needed) {
            continue;
        }
        if(*needed) {
            auto moved = placement_past(unit, *placement, *needed, function);
            if(!moved) {
                continue;
            }
            placement = moved;
        }
        placed.push_back(function);
    }

    auto pieces = [&](llvm::ArrayRef<const clang::FunctionDecl*> functions,
                      const clang::DeclContext* from,
                      bool host) {
        std::vector<DefinitionPiece> pieces;
        for(const auto* function: functions) {
            bool header_once = ctx.main_is_header && defined_once(function);
            if(host && !header_once) {
                continue;
            }
            if(auto text =
                   definition_text(unit, function, from, {.mark_inline = !host && header_once})) {
                pieces.push_back({unit.entity(function), std::move(*text)});
            }
        }
        return pieces;
    };

    auto title = std::format("Define missing members of '{}'", record->getName());
    if(placement) {
        if(auto same_file = pieces(placed, placement->from, false); !same_file.empty()) {
            out.push_back(define_action(title, at_placement(*placement, std::move(same_file))));
        }
    }
    if(ctx.main_is_header) {
        if(auto host = pieces(missing, unit.tu(), true); !host.empty()) {
            out.push_back(define_action(title,
                                        DefineInHostRequest{
                                            .container = unit.entity(outermost_record(record)),
                                            .pieces = std::move(host),
                                        }));
        }
    }
}

}  // namespace clice::feature::action
