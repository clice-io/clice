#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/feature.h"
#include "semantic/decls.h"
#include "semantic/semantics.h"
#include "semantic/symbol.h"
#include "syntax/lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/Module.h"

namespace clice::feature {

namespace {

/// The classification of one token: a kind and its modifiers.
struct Classified {
    SymbolKind kind = SymbolKind::Invalid;
    std::uint32_t modifiers = 0;
};

/// Merge a candidate into the running classification: differing kinds
/// collapse to Conflict, and only the modifiers every candidate agrees on
/// survive — a token combining resolutions from several instantiations
/// must not depend on the order the instantiations were written in.
void combine(Classified& result, Classified candidate) {
    if(candidate.kind == SymbolKind::Invalid) {
        return;
    }
    if(result.kind == SymbolKind::Invalid) {
        result = candidate;
        return;
    }
    result.modifiers &= candidate.modifiers;
    if(result.kind != candidate.kind) {
        result.kind = SymbolKind::Conflict;
    }
}

/// Whether a declaration name is backed by source text that should be highlighted.
bool can_highlight_name(clang::DeclarationName name) {
    switch(name.getNameKind()) {
        case clang::DeclarationName::Identifier: {
            auto* info = name.getAsIdentifierInfo();
            return info && !info->getName().empty();
        }

        case clang::DeclarationName::CXXConstructorName:
        case clang::DeclarationName::CXXDestructorName: {
            return true;
        }

        case clang::DeclarationName::CXXConversionFunctionName:
        case clang::DeclarationName::CXXOperatorName:
        case clang::DeclarationName::CXXDeductionGuideName:
        case clang::DeclarationName::CXXLiteralOperatorName:
        case clang::DeclarationName::CXXUsingDirective:
        case clang::DeclarationName::ObjCZeroArgSelector:
        case clang::DeclarationName::ObjCOneArgSelector:
        case clang::DeclarationName::ObjCMultiArgSelector: {
            return false;
        }
    }

    std::unreachable();
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

/// The classification a decl occurrence contributes: the decl's symbol kind
/// plus modifiers derived from the decl and the occurrence role.
Classified classify_decl(const clang::NamedDecl* decl, RelationKind relation) {
    if(relation.isReference() && !can_highlight_name(decl->getDeclName())) {
        return {};
    }

    auto mask = [](SymbolModifiers::Kind kind) {
        return SymbolModifiers::to_mask(kind);
    };

    std::uint32_t modifiers = 0;
    if(relation.is_one_of(RelationKind::Definition)) {
        // todo: clangd add both Declaration and Definition modifiers for definitions.
        modifiers |= mask(SymbolModifiers::Definition);
    } else if(relation.is_one_of(RelationKind::Declaration)) {
        modifiers |= mask(SymbolModifiers::Declaration);
    }

    if(decls::is_templated(decl)) {
        modifiers |= mask(SymbolModifiers::Templated);
    }

    auto kind = SymbolKind::from(decl);

    // Apply attribute-style modifiers to the underlying declaration.
    // The attribute tests don't want to look at the template.
    if(const auto* template_decl = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
        if(const auto* templated_decl = template_decl->getTemplatedDecl())
            decl = templated_decl;
    }

    // TODO: add scope-based modifiers once the local model supports them.

    if(is_const(decl)) {
        modifiers |= mask(SymbolModifiers::Readonly);
    }
    if(is_static(decl)) {
        modifiers |= mask(SymbolModifiers::Static);
    }
    if(is_abstract(decl)) {
        modifiers |= mask(SymbolModifiers::Abstract);
    }
    if(is_virtual(decl)) {
        modifiers |= mask(SymbolModifiers::Virtual);
    }
    if(is_default_library(decl)) {
        modifiers |= mask(SymbolModifiers::DefaultLibrary);
    }
    if(decl->isDeprecated()) {
        modifiers |= mask(SymbolModifiers::Deprecated);
    }
    if(llvm::isa<clang::UnresolvedUsingValueDecl>(decl)) {
        modifiers |= mask(SymbolModifiers::DependentName);
    }
    if(llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(decl)) {
        modifiers |= mask(SymbolModifiers::ConstructorOrDestructor);
    }

    return {kind, modifiers};
}

/// Classifies every spelled token of the interested file in one ordered
/// pass over the semantic map: lexical kinds straight from the token kind,
/// macros/includes/imports/attributes from the owning SemanticNode, and
/// declaration names by collecting the decls anchored at the token from its
/// owner chain. Conflicts are settled on the spot and adjacent tokens of the
/// same kind merge as they are emitted, so no post-processing pass is needed.
///
/// Tokens inside macro definition bodies only get lexical kinds: highlighting
/// them from their expansions belongs to the future expansion-preview feature
/// (a virtual file rendering the expansion with full semantic tokens).
class SemanticTokensCollector {
public:
    explicit SemanticTokensCollector(CompilationUnitRef unit) :
        unit(unit), semantics(unit.semantics()), content(unit.interested_content()) {}

    auto collect() -> std::vector<SemanticToken> {
        precompute_module_declaration();
        precompute_semantics();
        scan_comments();

        auto spelled = semantics.spelled_tokens();
        for(std::uint32_t i = 0; i < spelled.size(); i++) {
            flush_comments(semantics.token_offset(i));
            classify(i, spelled[i]);
        }
        flush_comments(static_cast<std::uint32_t>(content.size()));

        return std::move(tokens);
    }

private:
    void classify(std::uint32_t index, const clang::syntax::Token& token) {
        auto offset = semantics.token_offset(index);
        LocalSourceRange range(offset, offset + token.length());

        /// A logical newline between tokens ends any directive context; a
        /// backslash-newline splice continues the directive.
        if(offset > previous_end && has_logical_newline(previous_end, offset)) {
            directive_context = DirectiveContext::None;
        }
        previous_end = range.end;

        /// The module declaration (`export module foo.bar;`), precomputed.
        if(auto it = module_tokens.find(index); it != module_tokens.end()) {
            emit(range, it->second, 0);
            return;
        }

        Classified lexical = classify_lexical(token, offset);
        Classified semantic;
        if(auto it = token_semantics.find(index); it != token_semantics.end()) {
            semantic = it->second;
        }

        /// Semantic classification beats the lexical directive kinds; any
        /// other disagreement is a Conflict, matching the historical rule.
        Classified result = semantic;
        if(result.kind == SymbolKind::Invalid) {
            result = lexical;
        } else if(lexical.kind != SymbolKind::Invalid && lexical.kind != SymbolKind::Directive &&
                  lexical.kind != SymbolKind::Header && lexical.kind != result.kind) {
            result.kind = SymbolKind::Conflict;
        }

        if(result.kind != SymbolKind::Invalid) {
            emit(range, result.kind, result.modifiers);
        }
    }

    /// Lexical classification from the token kind (the spelled stream is
    /// produced by a real lexer, so keywords are already resolved), plus a
    /// small state machine for preprocessor directive context.
    Classified classify_lexical(const clang::syntax::Token& token, std::uint32_t offset) {
        Classified lexical;

        switch(token.kind()) {
            case clang::tok::numeric_constant: lexical.kind = SymbolKind::Number; break;
            case clang::tok::char_constant:
            case clang::tok::wide_char_constant:
            case clang::tok::utf8_char_constant:
            case clang::tok::utf16_char_constant:
            case clang::tok::utf32_char_constant: lexical.kind = SymbolKind::Character; break;
            case clang::tok::string_literal:
            case clang::tok::wide_string_literal:
            case clang::tok::utf8_string_literal:
            case clang::tok::utf16_string_literal:
            case clang::tok::utf32_string_literal: lexical.kind = SymbolKind::String; break;
            case clang::tok::hash: {
                if(directive_context == DirectiveContext::None) {
                    lexical.kind = SymbolKind::Directive;
                    directive_context = DirectiveContext::AfterHash;
                }
                break;
            }
            default: {
                if(directive_context == DirectiveContext::AfterHash) {
                    /// The directive name right after `#`, e.g. `include`, `if`.
                    lexical.kind = SymbolKind::Directive;
                    auto spelling = content.substr(offset, token.length());
                    if(spelling == "include" || spelling == "include_next" ||
                       spelling == "import" || spelling == "embed") {
                        directive_context = DirectiveContext::InIncludeName;
                    } else if(spelling == "define") {
                        directive_context = DirectiveContext::AfterDefine;
                    } else {
                        directive_context = DirectiveContext::InDirective;
                    }
                } else if(directive_context == DirectiveContext::AfterDefine) {
                    /// The macro name of a #define. Also covers preamble
                    /// defines under a PCH, where no MacroDefine node exists
                    /// (the preamble's directives live in the PCH compile).
                    lexical.kind = SymbolKind::Macro;
                    directive_context = DirectiveContext::InDirective;
                } else if(clang::tok::getKeywordSpelling(token.kind())) {
                    lexical.kind = SymbolKind::Keyword;
                } else if(auto* punctuator = clang::tok::getPunctuatorSpelling(token.kind())) {
                    /// Alternative operator spellings (and, or, not, ...) lex
                    /// as their punctuator kinds but are written as words.
                    auto spelling = content.substr(offset, token.length());
                    if(!spelling.empty() && llvm::isAlpha(spelling.front()) &&
                       spelling != punctuator) {
                        lexical.kind = SymbolKind::Keyword;
                    }
                }
                break;
            }
        }

        /// The filename of an #include: either a string literal or the
        /// `<vector>` token sequence; adjacent merging joins the pieces.
        /// The header context ends with the filename, so directive operands
        /// after it (e.g. #embed parameters) keep their own classification.
        if(directive_context == DirectiveContext::InIncludeName &&
           lexical.kind != SymbolKind::Directive) {
            if(token.kind() == clang::tok::less) {
                directive_context = DirectiveContext::InAngledName;
                lexical = {SymbolKind::Header, 0};
            } else if(lexical.kind == SymbolKind::String) {
                directive_context = DirectiveContext::InDirective;
                lexical = {SymbolKind::Header, 0};
            } else {
                directive_context = DirectiveContext::InDirective;
            }
        } else if(directive_context == DirectiveContext::InAngledName) {
            if(token.kind() == clang::tok::greater) {
                directive_context = DirectiveContext::InDirective;
            }
            lexical = {SymbolKind::Header, 0};
        }

        return lexical;
    }

    /// Ownership-based classification: walk the owner chain, collecting
    /// preprocessor entities directly and declaration names anchored exactly
    /// at this token.
    /// Semantic classification per spelled token, computed in one pass over
    /// the node table. Iterating nodes and anchoring their names to tokens is
    /// linear; the previous per-token owner-chain walk degraded quadratically
    /// on pathological inputs (a macro expanding to tens of thousands of
    /// nodes attributes all of them to one spelled invocation token).
    /// The spelled token written at `location`, or none. Macro locations
    /// resolve to their spelling: names written as macro arguments
    /// classify the argument token itself.
    auto spelled_index(clang::SourceLocation location) -> std::optional<std::uint32_t> {
        if(location.isInvalid()) {
            return std::nullopt;
        }
        if(location.isMacroID()) {
            location = unit.spelling_location(location);
        }

        auto spelled = semantics.spelled_tokens();
        auto it = std::partition_point(
            spelled.begin(),
            spelled.end(),
            [&](const clang::syntax::Token& token) { return token.location() < location; });
        if(it == spelled.end() || it->location() != location) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - spelled.begin());
    }

    void precompute_semantics() {
        auto spelled = semantics.spelled_tokens();

        /// Anchor a candidate at the spelled token written at `location`.
        /// With allow_ignored, tokens preprocessed away (directive regions)
        /// still classify — preprocessor names live there; declaration names
        /// never do, which keeps macro definition bodies lexical.
        auto anchor =
            [&](clang::SourceLocation location, Classified candidate, bool allow_ignored) {
                if(candidate.kind == SymbolKind::Invalid) {
                    return;
                }
                auto index = spelled_index(location);
                if(!index) {
                    return;
                }
                if(!allow_ignored && semantics.token_preprocessed_away(*index)) {
                    return;
                }
                combine(token_semantics[*index], candidate);
            };

        /// Anchor a declaration-name candidate. A written name can only be
        /// spelled as an identifier or a destructor's `~`; an occurrence
        /// resolving to any other token names nothing written — an anonymous
        /// parameter's trailing `)`, the `[` of a structured binding, the
        /// `(` of a constructor call written as the type name, an operator
        /// declaration's `operator` keyword — and must not paint it.
        auto anchor_name = [&](clang::SourceLocation location, Classified candidate) {
            if(candidate.kind == SymbolKind::Invalid) {
                return;
            }
            auto index = spelled_index(location);
            if(!index) {
                return;
            }
            auto kind = spelled[*index].kind();
            if(kind != clang::tok::identifier && kind != clang::tok::tilde) {
                return;
            }
            if(semantics.token_preprocessed_away(*index)) {
                return;
            }
            combine(token_semantics[*index], candidate);
        };

        for(auto [entry_index, entry]: llvm::enumerate(semantics.node_entries())) {
            // Instantiated nodes deliberately classify too: they repeat the
            // pattern's locations, so a dependent name paints as its actual
            // resolution — and as Conflict when instantiations disagree.
            const SemanticNode& node = entry.node;
            switch(node.kind()) {
                case SemanticNode::Kind::MacroDefine: {
                    anchor(
                        node.get<MacroRef>()->loc,
                        {SymbolKind::Macro, SymbolModifiers::to_mask(SymbolModifiers::Definition)},
                        true);
                    break;
                }

                case SemanticNode::Kind::MacroReference:
                case SemanticNode::Kind::MacroUndef: {
                    anchor(node.get<MacroRef>()->loc, {SymbolKind::Macro, 0}, true);
                    break;
                }

                case SemanticNode::Kind::Include: {
                    anchor(node.get<Include>()->location, {SymbolKind::Directive, 0}, true);
                    break;
                }

                case SemanticNode::Kind::Import: {
                    auto* import = node.get<Import>();
                    anchor(import->location, {SymbolKind::Keyword, 0}, true);
                    for(auto location: import->name_locations) {
                        auto index = spelled_index(location);
                        if(!index) {
                            continue;
                        }
                        /// A partition import (`import :part;`) reports the
                        /// component location at its leading colon; the
                        /// written name is the next spelled token. The colon
                        /// itself stays unpainted, matching the module
                        /// declaration side.
                        if(spelled[*index].kind() == clang::tok::colon &&
                           *index + 1 < spelled.size()) {
                            *index += 1;
                        }
                        combine(token_semantics[*index], {SymbolKind::Module, 0});
                    }
                    break;
                }

                case SemanticNode::Kind::Attr: {
                    /// `final` and `override` are contextual keywords.
                    if(llvm::isa<clang::FinalAttr, clang::OverrideAttr>(node.get<clang::Attr>())) {
                        anchor(node.get<clang::Attr>()->getRange().getBegin(),
                               {SymbolKind::Keyword, 0},
                               false);
                    }
                    break;
                }

                default: {
                    for(auto& occurrence:
                        resolve_occurrences(semantics,
                                            static_cast<std::uint32_t>(entry_index),
                                            &unit.resolver())) {
                        anchor_name(occurrence.location,
                                    classify_decl(occurrence.decl, occurrence.kind));
                    }
                    break;
                }
            }
        }
    }

    /// The module declaration has no AST node or directive record; locate its
    /// tokens up front so the main pass can classify them in order.
    void precompute_module_declaration() {
        auto* mod = unit.context().getCurrentNamedModule();
        if(!mod) {
            return;
        }

        /// The global module fragment (`module;`) precedes DefinitionLoc and
        /// its contextual `module` lexes as a plain identifier.
        {
            auto spelled = semantics.spelled_tokens();
            if(!spelled.empty() && spelled.front().kind() == clang::tok::identifier &&
               spelled.size() > 1 && spelled[1].kind() == clang::tok::semi) {
                auto offset = semantics.token_offset(0);
                if(content.substr(offset, spelled.front().length()) == "module") {
                    module_tokens[0] = SymbolKind::Keyword;
                }
            }
        }

        /// `module :private;` — the private fragment's contextual `module`
        /// also lexes as a plain identifier. The identifier-colon-`private`
        /// token sequence is not valid C++ anywhere else, so locate it
        /// lexically like the global module fragment above. Tokens
        /// preprocessed away (a disabled branch, a macro body) spell no
        /// fragment and keep their lexical handling.
        {
            auto spelled = semantics.spelled_tokens();
            for(std::uint32_t i = 0; i + 2 < spelled.size(); i += 1) {
                if(spelled[i].kind() != clang::tok::identifier ||
                   spelled[i + 1].kind() != clang::tok::colon ||
                   spelled[i + 2].kind() != clang::tok::kw_private ||
                   semantics.token_preprocessed_away(i)) {
                    continue;
                }
                auto offset = semantics.token_offset(i);
                if(content.substr(offset, spelled[i].length()) == "module") {
                    module_tokens[i] = SymbolKind::Keyword;
                }
            }
        }

        auto def_loc = mod->DefinitionLoc;
        if(!def_loc.isValid() || !def_loc.isFileID() ||
           unit.file_id(def_loc) != unit.interested_file()) {
            return;
        }

        auto spelled = semantics.spelled_tokens();
        auto count = static_cast<std::uint32_t>(spelled.size());
        std::uint32_t i = 0;
        while(i < count && spelled[i].location() < def_loc) {
            i++;
        }

        /// `module`, then the dotted name parts until the semicolon.
        if(i < count && spelled[i].kind() == clang::tok::identifier) {
            module_tokens[i] = SymbolKind::Keyword;
            i++;
        }
        for(; i < count && spelled[i].kind() != clang::tok::semi; i++) {
            if(spelled[i].kind() == clang::tok::identifier) {
                module_tokens[i] = SymbolKind::Module;
            }
        }
    }

    /// The spelled token stream does not retain comments; one slim raw scan
    /// collects them (and nothing else).
    void scan_comments() {
        auto& lang_opts = unit.lang_options();
        Lexer lexer(content, false, &lang_opts);

        while(true) {
            Token token = lexer.advance();
            if(token.is_eof()) {
                break;
            }

            if(token.kind == clang::tok::comment) {
                comments.push_back(token.range);
            }
        }
    }

    void flush_comments(std::uint32_t until) {
        while(next_comment < comments.size() && comments[next_comment].begin < until) {
            emit(comments[next_comment], SymbolKind::Comment, 0);
            next_comment++;
        }
    }

    bool has_logical_newline(std::uint32_t begin, std::uint32_t end) {
        /// Comment ranges come from the Lexer scan; gaps are visited in
        /// order, so one monotonic cursor suffices.
        auto inside_comment = [&](std::uint32_t offset) {
            while(newline_scan_comment < comments.size() &&
                  comments[newline_scan_comment].end <= offset) {
                newline_scan_comment += 1;
            }
            return newline_scan_comment < comments.size() &&
                   comments[newline_scan_comment].begin <= offset;
        };

        for(auto i = begin; i < end; i += 1) {
            /// A newline inside a comment is invisible to the preprocessor
            /// (a block comment is whitespace however many lines it spans;
            /// a line comment ends only at its unspliced newline, which the
            /// Lexer leaves outside the comment range).
            if(content[i] != '\n' || inside_comment(i)) {
                continue;
            }

            auto j = i;
            if(j > begin && content[j - 1] == '\r') {
                j -= 1;
            }
            if(j > begin && content[j - 1] == '\\') {
                continue;
            }
            return true;
        }
        return false;
    }

    void emit(LocalSourceRange range, SymbolKind kind, std::uint32_t modifiers) {
        if(!tokens.empty()) {
            auto& last = tokens.back();
            if(last.range.end == range.begin && last.kind == kind && last.modifiers == modifiers) {
                last.range.end = range.end;
                return;
            }
        }

        tokens.push_back({.range = range, .kind = kind, .modifiers = modifiers});
    }

    enum class DirectiveContext : std::uint8_t {
        None,
        AfterHash,
        InDirective,
        InIncludeName,
        InAngledName,
        AfterDefine,
    };

    CompilationUnitRef unit;
    const Semantics& semantics;
    llvm::StringRef content;
    DirectiveContext directive_context = DirectiveContext::None;
    std::uint32_t previous_end = 0;
    llvm::DenseMap<std::uint32_t, SymbolKind> module_tokens;
    llvm::DenseMap<std::uint32_t, Classified> token_semantics;
    std::vector<LocalSourceRange> comments;
    std::size_t next_comment = 0;
    /// Cursor of has_logical_newline over `comments`.
    std::size_t newline_scan_comment = 0;
    std::vector<SemanticToken> tokens;
};

class SemanticTokenEncoder {
public:
    SemanticTokenEncoder(CompilationUnitRef unit,
                         PositionEncoding encoding,
                         protocol::SemanticTokens& output) :
        map(unit.interested_content(), unit.line_starts(), encoding), encoding(encoding),
        output(output) {}

    void append(const SemanticToken& token) {
        auto content = map.content();
        if(!token.range.valid() || token.range.end <= token.range.begin ||
           token.range.end > content.size()) {
            return;
        }

        auto begin = token.range.begin;
        auto end = token.range.end;
        auto begin_position = to_position(map, begin);
        auto end_position = to_position(map, end);
        if(!begin_position || !end_position)
            return;
        auto begin_line = static_cast<std::uint32_t>(begin_position->line);
        auto begin_char = static_cast<std::uint32_t>(begin_position->character);
        auto end_line = static_cast<std::uint32_t>(end_position->line);
        auto end_char = static_cast<std::uint32_t>(end_position->character);

        if(begin_line == end_line) [[likely]] {
            emit(begin_line, begin_char, end_char - begin_char, token.kind, token.modifiers);
            return;
        }

        // LSP semantic tokens have no multiline support (unless the client
        // negotiates the capability), so split the token into per-line pieces.
        auto chunk = content.substr(begin, end - begin);
        std::uint32_t line = begin_line;
        std::uint32_t character = begin_char;
        std::uint32_t chunk_offset = 0;
        std::uint32_t piece_size = 0;

        for(char c: chunk) {
            piece_size += 1;
            if(c != '\n') {
                continue;
            }

            auto length = lsp::encoded_length(chunk.substr(chunk_offset, piece_size), encoding);
            emit(line, character, length, token.kind, token.modifiers);

            line += 1;
            character = 0;
            chunk_offset += piece_size;
            piece_size = 0;
        }

        if(piece_size > 0) {
            auto length = lsp::encoded_length(chunk.substr(chunk_offset), encoding);
            emit(line, character, length, token.kind, token.modifiers);
        }
    }

private:
    /// Emits one LSP entry at the absolute (line, character), computing the
    /// delta against the previously emitted entry. This is the single place
    /// that reads and updates the previous-position bookkeeping.
    void emit(std::uint32_t line,
              std::uint32_t character,
              std::uint32_t token_length,
              SymbolKind kind,
              std::uint32_t modifiers) {
        if(token_length == 0) {
            return;
        }

        auto delta_line = line - last_line;
        auto delta_start = delta_line == 0 ? character - last_start_character : character;
        output.data.push_back(delta_line);
        output.data.push_back(delta_start);
        output.data.push_back(token_length);
        output.data.push_back(kind.value_of());
        output.data.push_back(modifiers);

        last_line = line;
        last_start_character = character;
    }

private:
    lsp::LineMap map;
    PositionEncoding encoding;
    protocol::SemanticTokens& output;
    std::uint32_t last_line = 0;
    std::uint32_t last_start_character = 0;
};

}  // namespace

auto semantic_tokens(CompilationUnitRef unit) -> std::vector<SemanticToken> {
    SemanticTokensCollector collector(unit);
    return collector.collect();
}

auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding)
    -> protocol::SemanticTokens {
    auto tokens = semantic_tokens(unit);

    protocol::SemanticTokens result;
    result.data.reserve(tokens.size() * 5);

    SemanticTokenEncoder encoder(unit, encoding, result);
    for(const auto& token: tokens) {
        encoder.append(token);
    }

    return result;
}

}  // namespace clice::feature
