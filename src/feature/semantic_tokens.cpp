module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :feature.semantic_token_stream;
import :semantic.decls;
import :semantic.token_class;
import :semantic.resolver;
import :semantic.semantics;
import :semantic.symbol;
import :syntax.token;

namespace clice::feature {

namespace {

using Kind = SemanticTokenKind;
using Modifier = SemanticTokenModifier;

/// Whether a declaration name is backed by source text that should be highlighted.
bool can_highlight_name(clang::DeclarationName name) {
    switch(name.getNameKind()) {
        case clang::DeclarationName::Identifier: {
            auto* info = name.getAsIdentifierInfo();
            return info && !info->getName().empty();
        }

        case clang::DeclarationName::CXXConstructorName:
        case clang::DeclarationName::CXXDestructorName:
        case clang::DeclarationName::CXXOperatorName:
        case clang::DeclarationName::CXXLiteralOperatorName:
        case clang::DeclarationName::CXXDeductionGuideName: {
            return true;
        }

        case clang::DeclarationName::CXXConversionFunctionName:
        case clang::DeclarationName::CXXUsingDirective:
        case clang::DeclarationName::ObjCZeroArgSelector:
        case clang::DeclarationName::ObjCOneArgSelector:
        case clang::DeclarationName::ObjCMultiArgSelector: {
            return false;
        }
    }

    std::unreachable();
}

/// The name a dependent node writes that no candidate resolves, and what
/// its syntax says it is: a type after `typename`, the using-declaration a
/// dependent one declares, otherwise unknown.
std::optional<std::pair<clang::SourceLocation, TokenClass>>
    unresolved_name(const SemanticNode& node) {
    if(const auto* stmt = node.get<clang::Stmt>()) {
        if(const auto* name = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(stmt)) {
            return std::pair(name->getLocation(), TokenClass{.kind = Kind::Unknown});
        }
        if(const auto* member = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(stmt)) {
            return std::pair(member->getMemberLoc(), TokenClass{.kind = Kind::Unknown});
        }
        if(const auto* lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(stmt);
           lookup && lookup->getNumDecls() == 0) {
            return std::pair(lookup->getNameLoc(), TokenClass{.kind = Kind::Unknown});
        }
        if(const auto* member = llvm::dyn_cast<clang::UnresolvedMemberExpr>(stmt);
           member && member->getNumDecls() == 0) {
            return std::pair(member->getMemberLoc(), TokenClass{.kind = Kind::Unknown});
        }
        return std::nullopt;
    }
    if(const auto* type = node.get<clang::TypeLoc>()) {
        if(auto name = type->getAs<clang::DependentNameTypeLoc>()) {
            return std::pair(name.getNameLoc(), TokenClass{.kind = Kind::TypeAlias});
        }
        if(auto specialization = type->getAs<clang::TemplateSpecializationTypeLoc>();
           specialization &&
           specialization.getTypePtr()->getTemplateName().getAsDependentTemplateName()) {
            return std::pair(specialization.getTemplateNameLoc(),
                             TokenClass{.kind = Kind::Unknown});
        }
        return std::nullopt;
    }
    if(const auto* decl = node.get<clang::NamedDecl>();
       llvm::isa_and_present<clang::UnresolvedUsingValueDecl, clang::UnresolvedUsingTypenameDecl>(
           decl)) {
        auto classified = token_class(decl);
        classified.modifiers |= mask(Modifier::Declaration);
        return std::pair(decl->getLocation(), classified);
    }
    return std::nullopt;
}

/// The string literal a format string argument is written as, through
/// the conversions and the construction of a format string type.
const clang::StringLiteral* format_literal(const clang::Expr* argument) {
    while(argument) {
        argument = argument->IgnoreParenImpCasts();
        if(const auto* literal = llvm::dyn_cast<clang::StringLiteral>(argument)) {
            return literal;
        }
        if(const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(argument)) {
            argument = construct->getNumArgs() > 0 ? construct->getArg(0) : nullptr;
        } else if(const auto* temporary =
                      llvm::dyn_cast<clang::MaterializeTemporaryExpr>(argument)) {
            argument = temporary->getSubExpr();
        } else if(const auto* functional = llvm::dyn_cast<clang::CXXFunctionalCastExpr>(argument)) {
            argument = functional->getSubExpr();
        } else {
            return nullptr;
        }
    }
    return nullptr;
}

/// Whether `type` is a format string type: std::basic_format_string and
/// fmt's, which check a format string against its arguments.
bool is_format_string(clang::QualType type) {
    const auto* record = type.getNonReferenceType()->getAsCXXRecordDecl();
    if(!record || !record->getIdentifier()) {
        return false;
    }
    auto name = record->getName();
    return name == "basic_format_string" || name == "basic_fstring" || name == "fstring";
}

/// Collects what the AST says of the main file's tokens, for the token
/// stream: the classification of every written name, the format strings
/// and the arguments macros drop.
///
/// Tokens inside macro definition bodies get nothing: highlighting them
/// from their expansions belongs to the future expansion-preview feature
/// (a virtual file rendering the expansion with full semantic tokens).
class FactsCollector {
public:
    FactsCollector(CompilationUnit& unit,
                   llvm::ArrayRef<std::uint32_t> inactive_regions,
                   bool index_known) :
        unit(unit), semantics(unit.get<Semantics>()), sources(unit.context().getSourceManager()),
        inactive_regions(inactive_regions), index_known(index_known) {}

    auto collect() -> TokenFacts {
        for(auto [entry_index, entry]: llvm::enumerate(semantics.node_entries())) {
            visit(static_cast<std::uint32_t>(entry_index), entry);
        }
        for(auto& reference: references) {
            if(!declared.contains({reference.token, reference.decl})) {
                anchor_name(reference.location, reference.end, reference.classified);
            }
        }
        // What an instantiation resolves a dependent name to wins over
        // what the template's syntax says.
        for(auto [offset, classified]: unresolved) {
            if(!combined.contains(offset) && !owned.contains(offset)) {
                combined[offset] = classified;
            }
        }
        for(auto [offset, classified]: combined) {
            classified.modifiers |= usage.lookup(offset);
            facts.semantic.try_emplace(offset, classified);
        }
        for(auto [offset, classified]: owned) {
            classified.modifiers |= usage.lookup(offset);
            facts.semantic[offset] = classified;
        }
        if(!index_known) {
            collect_inactive();
        }
        return std::move(facts);
    }

private:
    void visit(std::uint32_t index, const Semantics::Node& entry) {
        // Instantiated nodes deliberately classify too: they repeat the
        // pattern's locations, so a dependent name paints as its actual
        // resolution — and as Conflict when instantiations disagree.
        const SemanticNode& node = entry.node;
        switch(node.kind()) {
            case SemanticNode::Kind::MacroReference:
            case SemanticNode::Kind::MacroUndef: {
                const auto* ref = node.get<MacroRef>();
                if(ref->macro) {
                    anchor(ref->loc, token_class(*ref->macro, sources), true);
                    if(ref->kind == MacroRef::Ref && ref->macro->isFunctionLike() &&
                       ref->loc.isFileID()) {
                        invocations.push_back(ref);
                    }
                }
                break;
            }

            case SemanticNode::Kind::Import: {
                for(auto location: node.get<Import>()->name_locations) {
                    anchor(location, {.kind = Kind::Module}, true);
                }
                break;
            }

            case SemanticNode::Kind::Module: {
                const auto* module = node.get<LexicalInfo::ModuleDeclaration>();
                TokenClass name{
                    .kind = Kind::Module,
                    .modifiers =
                        mask(unit.defines_module() ? Modifier::Definition : Modifier::Declaration),
                };
                for(auto& part: module->name_parts) {
                    combine(combined[part.begin], name);
                }
                if(module->kind == LexicalInfo::ModuleDeclaration::Kind::Declaration) {
                    for(auto& part: module->partition_parts) {
                        combine(combined[part.begin], name);
                    }
                }
                break;
            }

            // The token stream reads these from the text.
            case SemanticNode::Kind::MacroDefine:
            case SemanticNode::Kind::Include:
            case SemanticNode::Kind::Comment:
            case SemanticNode::Kind::Attr: {
                break;
            }

            default: {
                names(index, entry);
                if(index_known) {
                    break;
                }
                if(const auto* type = node.get<clang::TypeLoc>()) {
                    declarator(*type);
                }
                if(const auto* lambda =
                       llvm::dyn_cast_if_present<clang::LambdaExpr>(node.get<clang::Stmt>())) {
                    lambda_brackets(*lambda);
                }
                if(!entry.flags.in_instantiation) {
                    formats(node);
                }
                break;
            }
        }
    }

    /// The names node `index` writes, and the modifiers its use adds.
    void names(std::uint32_t index, const Semantics::Node& entry) {
        auto occurrences =
            resolve_occurrences(semantics, index, &unit.get<types::TemplateResolver>());
        if(occurrences.empty()) {
            // A dependent `co_yield` or `co_await` resolves through a
            // lookup located at its keyword.
            if(auto name = unresolved_name(entry.node); name && !index_known) {
                if(auto index = spelled_index(name->first);
                   index && semantics.spelled_tokens()[*index].kind() == clang::tok::identifier &&
                   !semantics.token_preprocessed_away(*index)) {
                    auto offset = semantics.token_offset(*index);
                    unresolved.try_emplace(offset, name->second);
                    usage[offset] |= mask(Modifier::Dependent);
                }
            }
            return;
        }
        // What an instantiation passes differs from one instantiation to
        // the next: the written code only.
        bool written = !entry.flags.in_instantiation && is_written(semantics, index);
        for(auto& occurrence: occurrences) {
            // `override`, `final` and `new` spell none of the declarations
            // they lead to.
            if(occurrence.kind.is_one_of(RelationKind::Implied)) {
                continue;
            }
            if(occurrence.kind.isReference() &&
               !can_highlight_name(occurrence.decl->getDeclName())) {
                continue;
            }
            const auto* decl = decls::normalize(occurrence.decl);
            auto classified = token_class(decl);
            auto end = occurrence.owns_whole_name() ? occurrence.name_end : clang::SourceLocation();
            auto token = spelled_index(occurrence.location);
            if(occurrence.kind.isDeclOrDef()) {
                classified.modifiers |= mask(occurrence.kind.is_one_of(RelationKind::Definition)
                                                 ? Modifier::Definition
                                                 : Modifier::Declaration);
                anchor_name(occurrence.location, end, classified);
                if(token) {
                    declared.insert({*token, decl->getCanonicalDecl()});
                }
            } else if(token) {
                // A using-declaration's targets are weak references too,
                // and depend on nothing. The instantiations resolving a
                // dependent name keep it dependent.
                if(occurrence.kind.is_one_of(RelationKind::WeakReference) && !index_known &&
                   !entry.node.get<clang::UsingDecl>()) {
                    usage[semantics.token_offset(*token)] |= mask(Modifier::Dependent);
                }
                references.push_back({
                    .location = occurrence.location,
                    .end = end,
                    .token = *token,
                    .decl = decl->getCanonicalDecl(),
                    .classified = classified,
                });
            }
            if(written) {
                if(auto offset = token_at(occurrence.location)) {
                    usage[*offset] |= mask(Modifier::Modification);
                }
            }
        }
    }

    /// The `*`, `&` and `&&` a declarator writes.
    void declarator(clang::TypeLoc type) {
        clang::SourceLocation location;
        if(auto pointer = type.getAs<clang::PointerTypeLoc>()) {
            location = pointer.getStarLoc();
        } else if(auto member = type.getAs<clang::MemberPointerTypeLoc>()) {
            location = member.getStarLoc();
        } else if(auto lvalue = type.getAs<clang::LValueReferenceTypeLoc>()) {
            location = lvalue.getAmpLoc();
        } else if(auto rvalue = type.getAs<clang::RValueReferenceTypeLoc>()) {
            location = rvalue.getAmpAmpLoc();
        }
        if(auto offset = token_at(location)) {
            owned[*offset] = {.kind = Kind::Operator, .modifiers = mask(Modifier::Declarator)};
        }
    }

    /// The brackets around a lambda's template parameters, which follow its
    /// captures with nothing in the text saying a template opens.
    void lambda_brackets(const clang::LambdaExpr& lambda) {
        if(lambda.getExplicitTemplateParameters().empty()) {
            return;
        }
        const auto* parameters = lambda.getTemplateParameterList();
        for(auto location: {parameters->getLAngleLoc(), parameters->getRAngleLoc()}) {
            if(auto offset = token_at(location)) {
                owned[*offset] = {.kind = Kind::Bracket};
            }
        }
    }

    /// The format strings a call passes: to a format string parameter
    /// (`std::format`), or where the callee's format attribute says
    /// (`printf`).
    void formats(const SemanticNode& node) {
        const auto* call = llvm::dyn_cast_if_present<clang::CallExpr>(node.get<clang::Stmt>());
        if(!call) {
            return;
        }
        auto mark = [&](const clang::Expr* argument, FormatStyle style) {
            const auto* literal = format_literal(argument);
            if(!literal) {
                return;
            }
            for(unsigned i = 0; i < literal->getNumConcatenated(); i += 1) {
                // A macro's replacement list stays lexical: one written
                // there formats only where the macro expands.
                auto location = literal->getStrTokenLoc(i);
                if(location.isMacroID() && !sources.isMacroArgExpansion(location)) {
                    continue;
                }
                if(auto offset = token_at(location)) {
                    facts.formats.try_emplace(*offset, style);
                }
            }
        };
        const auto* callee = call->getDirectCallee();
        if(!callee) {
            return;
        }
        for(const auto& binding: decls::parameter_bindings(call)) {
            if(is_format_string(binding.forwarded->getType())) {
                mark(binding.argument, FormatStyle::Std);
            }
        }
        for(const auto* attr: callee->specific_attrs<clang::FormatAttr>()) {
            auto type = attr->getType()->getName();
            auto style = type.contains("printf")  ? FormatStyle::Printf
                         : type.contains("scanf") ? FormatStyle::Scanf
                                                  : std::optional<FormatStyle>();
            if(!style) {
                continue;
            }
            // The index counts from 1, past an implicit object parameter,
            // which only an operator call's arguments hold.
            auto argument = attr->getFormatIdx() - 1;
            if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
               method && method->isImplicitObjectMemberFunction() &&
               !llvm::isa<clang::CXXOperatorCallExpr>(call)) {
                argument -= 1;
            }
            if(argument >= 0 && static_cast<unsigned>(argument) < call->getNumArgs()) {
                mark(call->getArg(argument), *style);
            }
        }
    }

    /// The inactive regions, merged with the arguments macro invocations
    /// drop: those of the parameters their definition never names.
    void collect_inactive() {
        auto spelled = semantics.spelled_tokens();
        std::vector<std::pair<std::uint32_t, std::uint32_t>> dropped;
        for(const auto* ref: invocations) {
            auto name = spelled_index(ref->loc);
            if(!name || *name + 1 >= spelled.size() ||
               spelled[*name + 1].kind() != clang::tok::l_paren) {
                continue;
            }
            const auto& macro = *ref->macro;
            auto used = [&](const clang::IdentifierInfo* parameter) {
                return llvm::any_of(macro.tokens(), [&](const clang::Token& token) {
                    return token.getIdentifierInfo() == parameter;
                });
            };
            // Each argument's first and last token; past the last named
            // parameter, a variadic macro's arguments are one.
            std::uint32_t parameter = 0;
            std::optional<std::uint32_t> first;
            std::uint32_t depth = 0;
            for(auto index = *name + 2; index < spelled.size(); index += 1) {
                auto kind = spelled[index].kind();
                bool ends = depth == 0 &&
                            (kind == clang::tok::r_paren ||
                             (kind == clang::tok::comma && parameter + 1 < macro.getNumParams()));
                if(ends) {
                    if(first && parameter < macro.getNumParams() &&
                       !used(macro.params()[parameter])) {
                        const auto& last = spelled[index - 1];
                        dropped.emplace_back(semantics.token_offset(*first),
                                             semantics.token_offset(index - 1) + last.length());
                    }
                    if(kind == clang::tok::r_paren) {
                        break;
                    }
                    parameter += 1;
                    first.reset();
                    continue;
                }
                if(kind == clang::tok::l_paren) {
                    depth += 1;
                } else if(kind == clang::tok::r_paren) {
                    depth -= 1;
                }
                if(!first) {
                    first = index;
                }
            }
        }
        // An argument may hold a whole untaken branch: overlapping spans
        // join.
        for(std::size_t i = 0; i + 1 < inactive_regions.size(); i += 2) {
            dropped.emplace_back(inactive_regions[i], inactive_regions[i + 1]);
        }
        std::ranges::sort(dropped);
        for(auto [begin, end]: dropped) {
            if(!facts.inactive.empty() && begin <= facts.inactive.back()) {
                facts.inactive.back() = std::max(facts.inactive.back(), end);
            } else {
                facts.inactive.push_back(begin);
                facts.inactive.push_back(end);
            }
        }
    }

    /// The offset of the spelled token written at `location`, macro
    /// locations at their spelling.
    std::optional<std::uint32_t> token_at(clang::SourceLocation location) {
        if(auto index = spelled_index(location)) {
            return semantics.token_offset(*index);
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> spelled_index(clang::SourceLocation location) {
        auto index = spelled_containing(location);
        if(!index || semantics.spelled_tokens()[*index].location() !=
                         (location.isMacroID() ? sources.getSpellingLoc(location) : location)) {
            return std::nullopt;
        }
        return index;
    }

    /// The spelled token `location` falls in, macro locations at their
    /// spelling: a literal operator's name ends inside its `""_kb` token.
    std::optional<std::uint32_t> spelled_containing(clang::SourceLocation location) {
        if(location.isInvalid()) {
            return std::nullopt;
        }
        if(location.isMacroID()) {
            location = sources.getSpellingLoc(location);
        }
        auto spelled = semantics.spelled_tokens();
        auto it = std::partition_point(
            spelled.begin(),
            spelled.end(),
            [&](const clang::syntax::Token& token) { return !(location < token.location()); });
        if(it == spelled.begin()) {
            return std::nullopt;
        }
        it -= 1;
        if(location.getRawEncoding() - it->location().getRawEncoding() >= it->length()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - spelled.begin());
    }

    /// Anchor a preprocessor name at the token written at `location`; with
    /// allow_ignored, tokens preprocessed away (directive lines) too.
    void anchor(clang::SourceLocation location, TokenClass classified, bool allow_ignored) {
        auto index = spelled_index(location);
        if(!index || (!allow_ignored && semantics.token_preprocessed_away(*index))) {
            return;
        }
        combine(combined[semantics.token_offset(*index)], classified);
    }

    /// Anchor a declaration name at `location` and, through `end` for a
    /// name of several tokens its declaration owns whole (`~Widget`), at
    /// every token of it names_token takes, over the names nested in it.
    void anchor_name(clang::SourceLocation location,
                     clang::SourceLocation end,
                     TokenClass classified) {
        if(classified.kind == Kind::None) {
            return;
        }
        auto first = spelled_index(location);
        if(!first) {
            return;
        }
        // A name pasted together in a macro body spans nothing written.
        auto last = end.isValid() ? spelled_containing(end) : first;
        if(!last || *last < *first || semantics.token_preprocessed_away(*first) ||
           semantics.token_preprocessed_away(*last)) {
            last = first;
        }
        auto spelled = semantics.spelled_tokens();
        for(auto index = *first; index <= *last; index += 1) {
            if(!names_token(spelled[index].kind(), classified) ||
               semantics.token_preprocessed_away(index)) {
                continue;
            }
            auto offset = semantics.token_offset(index);
            if(*last != *first) {
                owned[offset] = classified;
            } else {
                combine(combined[offset], classified);
            }
        }
    }

    CompilationUnit& unit;
    const Semantics& semantics;
    const clang::SourceManager& sources;
    llvm::ArrayRef<std::uint32_t> inactive_regions;
    bool index_known;

    TokenFacts facts;
    /// The candidates each token's names give, combined.
    llvm::DenseMap<std::uint32_t, TokenClass> combined;
    /// What a name of several tokens gives each of them, over the
    /// candidates.
    llvm::DenseMap<std::uint32_t, TokenClass> owned;
    /// Modifiers a token's use adds to whatever names it.
    llvm::DenseMap<std::uint32_t, std::uint32_t> usage;

    struct Reference {
        clang::SourceLocation location;
        clang::SourceLocation end;
        std::uint32_t token;
        const clang::Decl* decl;
        TokenClass classified;
    };

    /// The references, anchored once every declaration is: one to a
    /// declaration written at the same token (the class a variable's
    /// type defines, `struct Point {} origin;`) is that declaration and
    /// adds nothing.
    std::vector<Reference> references;
    llvm::DenseSet<std::pair<std::uint32_t, const clang::Decl*>> declared;

    /// The function-like macro invocations written in the file.
    std::vector<const MacroRef*> invocations;

    /// The names no candidate resolves, by what their syntax says.
    llvm::DenseMap<std::uint32_t, TokenClass> unresolved;
};

std::string lower_first(llvm::StringRef name) {
    auto result = name.str();
    if(!result.empty()) {
        result[0] = static_cast<char>(llvm::toLower(result[0]));
    }
    return result;
}

class SemanticTokenEncoder {
public:
    SemanticTokenEncoder(const PositionMap& map,
                         PositionEncoding encoding,
                         const SemanticTokenLegend& legend,
                         protocol::SemanticTokens& output) :
        map(map), encoding(encoding), legend(legend), output(output) {}

    void append(const SemanticToken& token, std::optional<LocalSourceRange> window) {
        if(token.range.end <= token.range.begin || token.range.end > map.size()) {
            return;
        }
        auto type = legend.type_of[static_cast<std::size_t>(token.kind)];
        if(!type) {
            return;
        }
        auto modifiers = legend.bits(token.modifiers);

        if(window && (token.range.end <= window->begin || token.range.begin >= window->end)) {
            return;
        }
        auto begin = map.position(token.range.begin, encoding);
        auto end = map.position(token.range.end, encoding);

        // LSP semantic tokens have no multiline support (unless the client
        // negotiates the capability), so split the token into per-line pieces.
        for(auto line = begin->line; line <= end->line; line += 1) {
            auto bounds = *map.line_bounds(line);
            if(window &&
               ((line == end->line ? token.range.end : bounds.end) <= window->begin ||
                (line == begin->line ? token.range.begin : bounds.begin) >= window->end)) {
                continue;
            }
            auto start = line == begin->line ? begin->character : 0;
            auto stop =
                line == end->line ? end->character : map.position(bounds.end, encoding)->character;
            emit(line, start, stop - start, *type, modifiers);
        }
    }

private:
    /// Emits one LSP entry at the absolute (line, character), computing the
    /// delta against the previously emitted entry. This is the single place
    /// that reads and updates the previous-position bookkeeping.
    void emit(std::uint32_t line,
              std::uint32_t character,
              std::uint32_t token_length,
              std::uint32_t type,
              std::uint32_t modifiers) {
        if(token_length == 0) {
            return;
        }

        auto delta_line = line - last_line;
        auto delta_start = delta_line == 0 ? character - last_start_character : character;
        output.data.push_back(delta_line);
        output.data.push_back(delta_start);
        output.data.push_back(token_length);
        output.data.push_back(type);
        output.data.push_back(modifiers);

        last_line = line;
        last_start_character = character;
    }

    const PositionMap& map;
    PositionEncoding encoding;
    const SemanticTokenLegend& legend;
    protocol::SemanticTokens& output;
    std::uint32_t last_line = 0;
    std::uint32_t last_start_character = 0;
};

}  // namespace

std::string legend_name(SemanticTokenKind kind) {
    return lower_first(kota::meta::enum_name(kind));
}

std::string legend_name(SemanticTokenModifier modifier) {
    return lower_first(kota::meta::enum_name(modifier));
}

auto semantic_token_legend(const SemanticTokensOptions& options) -> SemanticTokenLegend {
    SemanticTokenLegend legend;
    auto renamed = [&](const std::map<std::string, std::string>& names, std::string name) {
        auto it = names.find(name);
        return it == names.end() ? name : it->second;
    };
    auto position = [](std::vector<std::string>& names, const std::string& name) {
        auto it = llvm::find(names, name);
        if(it == names.end()) {
            names.push_back(name);
            return static_cast<std::uint32_t>(names.size() - 1);
        }
        return static_cast<std::uint32_t>(it - names.begin());
    };

    llvm::StringSet<> known;
    for(auto kind: kota::meta::reflection<SemanticTokenKind>::member_values) {
        auto name = legend_name(kind);
        known.insert(name);
        if(kind == SemanticTokenKind::None) {
            continue;
        }
        if(auto sent = renamed(options.kinds, name); !sent.empty()) {
            legend.type_of[static_cast<std::size_t>(kind)] = position(legend.types, sent);
        }
    }
    for(auto& [name, sent]: options.kinds) {
        if(!known.contains(name) || name == "none") {
            legend.unknown.push_back(name);
        }
    }

    known.clear();
    for(auto modifier: kota::meta::reflection<SemanticTokenModifier>::member_values) {
        auto name = legend_name(modifier);
        known.insert(name);
        if(auto sent = renamed(options.modifiers, name); !sent.empty()) {
            legend.bit_of[static_cast<std::size_t>(modifier)] = std::uint32_t(1)
                                                                << position(legend.modifiers, sent);
        }
    }
    for(auto& [name, sent]: options.modifiers) {
        if(!known.contains(name)) {
            legend.unknown.push_back(name);
        }
    }
    return legend;
}

auto semantic_token_facts(CompilationUnit& unit,
                          llvm::ArrayRef<std::uint32_t> inactive_regions,
                          bool index_known) -> TokenFacts {
    return FactsCollector(unit, inactive_regions, index_known).collect();
}

auto semantic_tokens(CompilationUnit& unit,
                     llvm::ArrayRef<std::uint32_t> inactive_regions,
                     const SemanticTokensOptions& options) -> std::vector<SemanticToken> {
    auto facts = semantic_token_facts(unit, inactive_regions, false);
    auto focus = unit.focus_file();
    // A copy: the lexer reads up to a NUL terminator, which a prefix lacks.
    auto text = focus.content().take_front(unit.text_end(focus.file())).str();
    return semantic_token_stream(text, unit.lang_options(), facts, options);
}

auto semantic_tokens(CompilationUnit& unit, const SemanticTokensOptions& options)
    -> std::vector<SemanticToken> {
    auto scan = inactive_regions(unit);
    return semantic_tokens(unit, scan.regions, options);
}

auto semantic_tokens_to_protocol(llvm::ArrayRef<SemanticToken> tokens,
                                 const PositionMap& map,
                                 PositionEncoding encoding,
                                 const SemanticTokenLegend& legend,
                                 std::optional<LocalSourceRange> window)
    -> protocol::SemanticTokens {
    protocol::SemanticTokens result;
    result.data.reserve(tokens.size() * 5);

    SemanticTokenEncoder encoder(map, encoding, legend, result);
    for(const auto& token: tokens) {
        encoder.append(token, window);
    }

    return result;
}

auto semantic_tokens_edits(llvm::ArrayRef<std::uint32_t> before,
                           llvm::ArrayRef<std::uint32_t> after)
    -> std::vector<protocol::SemanticTokensEdit> {
    std::size_t prefix = 0;
    while(prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) {
        prefix += 1;
    }
    std::size_t suffix = 0;
    while(suffix < before.size() - prefix && suffix < after.size() - prefix &&
          before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        suffix += 1;
    }
    if(prefix == before.size() && prefix == after.size()) {
        return {};
    }
    auto inserted = after.slice(prefix, after.size() - prefix - suffix);
    return {
        {
         .start = static_cast<std::uint32_t>(prefix),
         .delete_count = static_cast<std::uint32_t>(before.size() - prefix - suffix),
         .data = std::vector<std::uint32_t>(inserted.begin(), inserted.end()),
         }
    };
}

}  // namespace clice::feature
