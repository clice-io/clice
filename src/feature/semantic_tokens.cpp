#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "semantic/semantic_visitor.h"
#include "semantic/symbol_kind.h"
#include "syntax/lexer.h"

#include "clang/AST/Attr.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/Module.h"

namespace clice::feature {

namespace {

struct RawToken {
    LocalSourceRange range;
    SymbolKind kind = SymbolKind::Invalid;
    std::uint32_t modifiers = 0;
};

void add_modifier(std::uint32_t& modifiers, SymbolModifiers::Kind kind) {
    modifiers |= SymbolModifiers::to_mask(kind);
}

auto type_index(SymbolKind kind) -> std::uint32_t {
    return kind.value_of();
}

auto encode_modifiers(std::uint32_t modifiers) -> std::uint32_t {
    return modifiers;
}

bool is_dependent(const clang::Decl* D) {
    return isa<clang::UnresolvedUsingValueDecl>(D);
}

/// Returns true if `decl` is considered to be from a default/system library.
/// This currently checks the systemness of the file by include type, although
/// different heuristics may be used in the future (e.g. sysroot paths).
bool is_default_library(const clang::Decl* decl) {
    clang::SourceLocation location = decl->getLocation();
    if(!location.isValid()) {
        return false;
    }
    return decl->getASTContext().getSourceManager().isInSystemHeader(location);
}

// "Static" means many things in C++, only some get the "static" modifier.
//
// Meanings that do:
// - Members associated with the class rather than the instance.
//   This is what 'static' most often means across languages.
// - static local variables
//   These are similarly "detached from their context" by the static keyword.
//   In practice, these are rarely used inside classes, reducing confusion.
//
// Meanings that don't:
// - Namespace-scoped variables, which have static storage class.
//   This is implicit, so the keyword "static" isn't so strongly associated.
//   If we want a modifier for these, "global scope" is probably the concept.
// - Namespace-scoped variables/functions explicitly marked "static".
//   There the keyword changes *linkage* , which is a totally different concept.
//   If we want to model this, "file scope" would be a nice modifier.
//
// This is confusing, and maybe we should use another name, but because "static"
// is a standard LSP modifier, having one with that name has advantages.
bool is_static(const clang::Decl* decl) {
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return method->isStatic();
    }
    if(const auto* var_decl = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return var_decl->isStaticDataMember() || var_decl->isStaticLocal();
    }
    if(const auto* objc_property = llvm::dyn_cast<clang::ObjCPropertyDecl>(decl)) {
        return objc_property->isClassProperty();
    }
    if(const auto* objc_method = llvm::dyn_cast<clang::ObjCMethodDecl>(decl)) {
        return objc_method->isClassMethod();
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->isStatic();
    }
    return false;
}

// Whether `type` is const in a loose sense: would a value of this type be readonly?
bool is_const(clang::QualType type) {
    if(type.isNull()) {
        return false;
    }
    type = type.getNonReferenceType();
    if(type.isConstQualified()) {
        return true;
    }
    if(const auto* array_type = type->getAsArrayTypeUnsafe()) {
        return is_const(array_type->getElementType());
    }
    if(is_const(type->getPointeeType())) {
        return true;
    }
    return false;
}

// Whether `decl` is const in a loose sense (should it be highlighted as such?)
// FIXME: This is separate from whether a particular usage can mutate `decl`.
//        We may want a receiver in `value.size()` to be readonly even if `value` is mutable.
bool is_const(const clang::Decl* decl) {
    if(llvm::isa<clang::EnumConstantDecl>(decl) ||
       llvm::isa<clang::NonTypeTemplateParmDecl>(decl)) {
        return true;
    }
    if(llvm::isa<clang::FieldDecl>(decl) || llvm::isa<clang::VarDecl>(decl) ||
       llvm::isa<clang::MSPropertyDecl>(decl) || llvm::isa<clang::BindingDecl>(decl)) {
        if(is_const(llvm::cast<clang::ValueDecl>(decl)->getType())) {
            return true;
        }
    }
    if(const auto* objc_property = llvm::dyn_cast<clang::ObjCPropertyDecl>(decl)) {
        if(objc_property->isReadOnly()) {
            return true;
        }
    }
    if(const auto* ms_property = llvm::dyn_cast<clang::MSPropertyDecl>(decl)) {
        if(!ms_property->hasSetter()) {
            return true;
        }
    }
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        if(method->isConst()) {
            return true;
        }
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return is_const(function->getReturnType());
    }
    return false;
}

// Indicates whether declaration `decl` is abstract in cases where it is a struct or a
// class.
bool is_abstract(const clang::Decl* decl) {
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return method->isPureVirtual();
    }
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        return record->hasDefinition() && record->isAbstract();
    }
    return false;
}

// Indicates whether declaration `decl` is virtual in cases where it is a method.
bool is_virtual(const clang::Decl* decl) {
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        return method->isVirtual();
    }
    return false;
}

class SemanticTokensCollector : public SemanticVisitor<SemanticTokensCollector> {
public:
    explicit SemanticTokensCollector(CompilationUnitRef unit) : SemanticVisitor(unit, true) {}

    auto collect() -> std::vector<RawToken> {
        highlight_lexical(unit.interested_file());
        run();
        highlight_modules();
        merge_tokens();
        return std::move(tokens);
    }

    void handleDeclOccurrence(const clang::NamedDecl* decl,
                              RelationKind relation,
                              clang::SourceLocation location) {
        std::uint32_t modifiers = 0;
        if(relation.is_one_of(RelationKind::Definition)) {
            // todo: clangd add both Declaration and Definition modifiers for definitions.
            // add_modifier(modifiers, SymbolModifiers::Declaration);
            add_modifier(modifiers, SymbolModifiers::Definition);
        } else if(relation.is_one_of(RelationKind::Declaration)) {
            add_modifier(modifiers, SymbolModifiers::Declaration);
        }

        if(ast::is_templated(decl)) {
            add_modifier(modifiers, SymbolModifiers::Templated);
        }
        // Apply attribute-style modifiers to the underlying declaration.
        // The attribute tests don't want to look at the template.
        if(const auto* template_decl = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            if(const auto* templated_decl = template_decl->getTemplatedDecl())
                decl = templated_decl;
        }

        // TODO: add scope-based modifiers once the local model supports them.
        // if (auto Mod = scopeModifier(Decl))
        //     Tok.addModifier(*Mod);

        if(is_const(decl)) {
            add_modifier(modifiers, SymbolModifiers::Readonly);
        }
        if(is_static(decl)) {
            add_modifier(modifiers, SymbolModifiers::Static);
        }
        if(is_abstract(decl)) {
            add_modifier(modifiers, SymbolModifiers::Abstract);
        }
        if(is_virtual(decl)) {
            add_modifier(modifiers, SymbolModifiers::Virtual);
        }
        if(is_default_library(decl)) {
            add_modifier(modifiers, SymbolModifiers::DefaultLibrary);
        }
        if(decl->isDeprecated()) {
            add_modifier(modifiers, SymbolModifiers::Deprecated);
        }
        if(is_dependent(decl)) {
            add_modifier(modifiers, SymbolModifiers::DependentName);
        }
        if(llvm::isa<clang::CXXConstructorDecl>(decl) ||
           llvm::isa<clang::CXXDestructorDecl>(decl)) {
            add_modifier(modifiers, SymbolModifiers::ConstructorOrDestructor);
        }

        add_token(location, SymbolKind::from(decl), modifiers);
    }

    void handleMacroOccurrence(const clang::MacroInfo*,
                               RelationKind relation,
                               clang::SourceLocation location) {
        std::uint32_t modifiers = 0;
        if(relation.is_one_of(RelationKind::Definition)) {
            add_modifier(modifiers, SymbolModifiers::Definition);
        } else if(relation.is_one_of(RelationKind::Declaration)) {
            add_modifier(modifiers, SymbolModifiers::Declaration);
        }

        add_token(location, SymbolKind::Macro, modifiers);
    }

    // handleModuleOccurrence

    // handleRelation

    void handleAttrOccurrence(const clang::Attr* attr, clang::SourceRange range) {
        auto [begin, end] = range;
        if(llvm::isa<clang::FinalAttr, clang::OverrideAttr>(attr)) {
            assert(begin == end && "attribute token should be one location");
            add_token(begin, SymbolKind::Keyword, 0);
        }
    }

private:
    void add_token(clang::FileID fid, Token token, SymbolKind kind, std::uint32_t modifiers) {
        if(fid != unit.interested_file() || kind == SymbolKind::Invalid) {
            return;
        }

        tokens.push_back({
            .range = token.range,
            .kind = kind,
            .modifiers = modifiers,
        });
    }

    void add_token(clang::SourceLocation location, SymbolKind kind, std::uint32_t modifiers) {
        if(kind == SymbolKind::Invalid) {
            return;
        }

        if(location.isMacroID()) {
            auto spelling = unit.spelling_location(location);
            auto expansion = unit.expansion_location(location);
            if(unit.file_id(spelling) != unit.file_id(expansion)) {
                return;
            }
            location = spelling;
        }

        auto [fid, range] = unit.decompose_range(location);
        if(fid != unit.interested_file()) {
            return;
        }

        tokens.push_back({
            .range = range,
            .kind = kind,
            .modifiers = modifiers,
        });
    }

    void highlight_modules() {
        auto interested = unit.interested_file();

        // Import statements: highlight module name identifiers.
        auto directives_it = unit.directives().find(interested);
        if(directives_it != unit.directives().end()) {
            for(const auto& import: directives_it->second.imports) {
                for(auto loc: import.name_locations) {
                    add_token(loc, SymbolKind::Module, 0);
                }
            }
        }

        // Module declaration (export module foo.bar;): highlight module name.
        auto* mod = unit.context().getCurrentNamedModule();
        if(!mod) {
            return;
        }

        auto def_loc = mod->DefinitionLoc;
        if(!def_loc.isValid() || !def_loc.isFileID()) {
            return;
        }

        auto [fid, offset] = unit.decompose_location(def_loc);
        if(fid != interested) {
            return;
        }

        auto content = unit.file_content(fid);
        auto& lang_opts = unit.lang_options();
        Lexer lexer(content.substr(offset), false, &lang_opts);

        // Skip 'module' keyword.
        lexer.advance();

        // Scan for identifiers (module name parts) until semicolon/eof.
        while(true) {
            auto token = lexer.advance();
            if(token.is_eof() || token.kind == clang::tok::semi) {
                break;
            }
            if(token.is_identifier()) {
                auto range = LocalSourceRange(offset + token.range.begin, offset + token.range.end);
                tokens.push_back({.range = range, .kind = SymbolKind::Module, .modifiers = 0});
            }
        }
    }

    void highlight_lexical(clang::FileID fid) {
        auto content = unit.file_content(fid);
        auto& lang_opts = unit.lang_options();
        clang::IdentifierTable identifiers(lang_opts);
        Lexer lexer(content, false, &lang_opts);

        while(true) {
            Token token = lexer.advance();
            if(token.is_eof()) {
                break;
            }

            SymbolKind kind = SymbolKind::Invalid;

            if(token.is_directive_hash() || token.is_pp_keyword) {
                kind = SymbolKind::Directive;
            } else {
                switch(token.kind) {
                    case clang::tok::comment: kind = SymbolKind::Comment; break;
                    case clang::tok::numeric_constant: kind = SymbolKind::Number; break;
                    case clang::tok::char_constant:
                    case clang::tok::wide_char_constant:
                    case clang::tok::utf8_char_constant:
                    case clang::tok::utf16_char_constant:
                    case clang::tok::utf32_char_constant: kind = SymbolKind::Character; break;
                    case clang::tok::string_literal:
                    case clang::tok::wide_string_literal:
                    case clang::tok::utf8_string_literal:
                    case clang::tok::utf16_string_literal:
                    case clang::tok::utf32_string_literal: kind = SymbolKind::String; break;
                    case clang::tok::header_name: kind = SymbolKind::Header; break;
                    case clang::tok::raw_identifier: {
                        auto previous = lexer.last();
                        if(previous.is_pp_keyword && previous.text(content) == "define") {
                            kind = SymbolKind::Macro;
                            break;
                        }

                        auto spelling = token.text(content);
                        if(identifiers.get(spelling).isKeyword(lang_opts)) {
                            kind = SymbolKind::Keyword;
                        }
                        break;
                    }

                    default: break;
                }
            }

            add_token(fid, token, kind, 0);
        }
    }

    static void resolve_conflict(RawToken& last, const RawToken& current) {
        (void)current;
        if(last.kind == SymbolKind::Conflict) {
            return;
        }
        last.kind = SymbolKind::Conflict;
    }

    void merge_tokens() {
        std::ranges::sort(tokens, [](const RawToken& lhs, const RawToken& rhs) {
            if(lhs.range.begin != rhs.range.begin) {
                return lhs.range.begin < rhs.range.begin;
            }
            return lhs.range.end < rhs.range.end;
        });

        std::vector<RawToken> merged;
        merged.reserve(tokens.size());

        for(const auto& token: tokens) {
            if(merged.empty()) {
                merged.push_back(token);
                continue;
            }

            auto& last = merged.back();
            if(last.range == token.range) {
                resolve_conflict(last, token);
                continue;
            }

            if(last.range.end == token.range.begin && last.kind == token.kind) {
                last.range.end = token.range.end;
                continue;
            }

            merged.push_back(token);
        }

        tokens = std::move(merged);
    }

public:
    std::vector<RawToken> tokens;
};

class SemanticTokenEncoder {
public:
    SemanticTokenEncoder(llvm::StringRef content,
                         PositionEncoding encoding,
                         protocol::SemanticTokens& output) :
        content(content), converter(content, encoding), output(output) {}

    void append(const RawToken& token) {
        if(!token.range.valid() || token.range.end <= token.range.begin ||
           token.range.end > content.size()) {
            return;
        }

        auto begin = token.range.begin;
        auto end = token.range.end;
        auto begin_position = *converter.to_position(begin);
        auto end_position = *converter.to_position(end);
        auto begin_line = static_cast<std::uint32_t>(begin_position.line);
        auto begin_char = static_cast<std::uint32_t>(begin_position.character);
        auto end_line = static_cast<std::uint32_t>(end_position.line);
        auto end_char = static_cast<std::uint32_t>(end_position.character);

        if(begin_line == end_line) [[likely]] {
            auto delta_line = begin_line - last_line;
            auto delta_start = delta_line == 0 ? begin_char - last_start_character : begin_char;
            auto token_length = end_char - begin_char;
            emit_relative(delta_line, delta_start, token_length, token.kind, token.modifiers);
        } else {
            auto chunk = content.substr(begin, end - begin);
            bool first_piece = true;
            std::uint32_t chunk_offset = 0;
            std::uint32_t piece_size = 0;

            for(char c: chunk) {
                piece_size += 1;
                if(c != '\n') {
                    continue;
                }

                std::uint32_t delta_line = 1;
                std::uint32_t delta_start = 0;
                if(first_piece) {
                    delta_line = begin_line - last_line;
                    delta_start = delta_line == 0 ? begin_char - last_start_character : begin_char;
                    first_piece = false;
                }

                auto length = converter.measure(chunk.substr(chunk_offset, piece_size));
                emit_relative(delta_line, delta_start, length, token.kind, token.modifiers);

                chunk_offset += piece_size;
                piece_size = 0;
            }

            if(piece_size > 0) {
                auto length = converter.measure(chunk.substr(chunk_offset));
                emit_relative(1, 0, length, token.kind, token.modifiers);
            }
        }

        last_line = end_line;
        last_start_character = begin_char;
    }

private:
    void emit_relative(std::uint32_t delta_line,
                       std::uint32_t delta_start,
                       std::uint32_t token_length,
                       SymbolKind kind,
                       std::uint32_t modifiers) {
        if(token_length == 0) {
            return;
        }

        output.data.push_back(delta_line);
        output.data.push_back(delta_start);
        output.data.push_back(token_length);
        output.data.push_back(type_index(kind));
        output.data.push_back(encode_modifiers(modifiers));
    }

private:
    llvm::StringRef content;
    PositionMapper converter;
    protocol::SemanticTokens& output;
    std::uint32_t last_line = 0;
    std::uint32_t last_start_character = 0;
};

}  // namespace

auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding)
    -> protocol::SemanticTokens {
    SemanticTokensCollector collector(unit);
    auto tokens = collector.collect();

    protocol::SemanticTokens result;
    result.data.reserve(tokens.size() * 5);

    SemanticTokenEncoder encoder(unit.interested_content(), encoding, result);
    for(const auto& token: tokens) {
        encoder.append(token);
    }

    return result;
}

}  // namespace clice::feature
