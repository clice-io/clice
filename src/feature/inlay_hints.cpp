module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.semantics;
import :feature.feature;
import :index.tu_index;
import :index.types;
import :semantic.decls;
import :semantic.display;
import :semantic.resolver;
import :semantic.types;

namespace clice::feature {

namespace {

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

// For now, inlay hints are always anchored at the left or right of their range.
enum class HintSide : std::uint8_t { Left, Right };

// for a ParmVarDecl from a function declaration, returns the corresponding
// ParmVarDecl from the definition if possible, nullptr otherwise.
const clang::ParmVarDecl* param_definition(const clang::ParmVarDecl* param) {
    if(auto* callee = dyn_cast<clang::FunctionDecl>(param->getDeclContext())) {
        if(auto* def = callee->getDefinition()) {
            auto i = std::distance(callee->param_begin(), llvm::find(callee->parameters(), param));
            if(i < (int)callee->getNumParams()) {
                return def->getParamDecl(i);
            }
        }
    }
    return nullptr;
}

bool is_setter(const clang::FunctionDecl* callee,
               const llvm::SmallVector<llvm::StringRef, 8>& names) {
    if(names.size() != 1) {
        return false;
    }

    llvm::StringRef name = display::identifier_of(callee);
    if(!name.starts_with_insensitive("set")) {
        return false;
    }

    // In addition to checking that the function has one parameter and its
    // name starts with "set", also check that the part after "set" matches
    // the name of the parameter (ignoring case). The idea here is that if
    // the parameter name differs, it may contain extra information that
    // may be useful to show in a hint, as in:
    //   void setTimeout(int timeoutMillis);
    // This currently doesn't handle cases where params use snake_case
    // and functions don't, e.g.
    //   void setExceptionHandler(EHFunc exception_handler);
    // We could improve this by replacing `equals_insensitive` with some
    // `sloppy_equals` which ignores case and also skips underscores.
    return name.substr(3).ltrim("_").equals_insensitive(names[0]);
}

// Checks if the callee is one of the builtins
// addressof, as_const, forward, move(_if_noexcept)
//
// The real criterion is "so common and so obvious that a parameter hint is
// pure noise", and neither the builtin-ID check nor the name fallback below
// expresses it well: the ID set is whatever clang happens to fold, and the
// single-parameter guard only shields the std::move algorithm overload.
// FIXME: Replace both with a curated suppression table (qualified name +
// signature shape) so the set is explicit and extensible beyond cast-like
// single-parameter forms.
bool is_simple_builtin(const clang::FunctionDecl* callee) {
    switch(callee->getBuiltinID()) {
        case clang::Builtin::BIaddressof:
        case clang::Builtin::BIas_const:
        case clang::Builtin::BIforward:
        case clang::Builtin::BIforward_like:
        case clang::Builtin::BImove:
        case clang::Builtin::BImove_if_noexcept: return true;
        default: break;
    }

    // Freestanding compiles (-ffreestanding) strip library-builtin IDs, so
    // match the same set by name; their arguments stay uninteresting either
    // way. All of these are single-parameter cast-like forms — the check
    // must not swallow e.g. the three-argument std::move algorithm, whose
    // parameters are worth hinting.
    if(callee->getNumParams() == 1 && callee->isInStdNamespace()) {
        llvm::StringRef name = display::identifier_of(callee);
        return name == "addressof" || name == "as_const" || name == "forward" ||
               name == "forward_like" || name == "move" || name == "move_if_noexcept";
    }

    return false;
}

struct Callee {
    // Only one of Decl or Loc is set.
    // Loc is for calls through function pointers.
    const clang::FunctionDecl* decl = nullptr;
    clang::FunctionProtoTypeLoc loc;
};

/// A name a printed type spells, and the declaration a link on it reaches.
struct SpelledName {
    llvm::StringRef name;
    const clang::NamedDecl* decl;
};

/// The names TypePrinter writes for declarations when it prints a type, in
/// print order: classes, enums, aliases and templates, inside template
/// arguments and qualifiers too. The walk follows the printer where it
/// decides what is written — preferred names, defaulted arguments left out —
/// and a name it misjudges is merely left unlinked.
struct SpelledNames {
    llvm::SmallVector<SpelledName, 4> names;

    void walk(clang::QualType type) {
        if(type.isNull()) {
            return;
        }
        const clang::Type* T = type.getTypePtr();
        if(auto* typedef_type = llvm::dyn_cast<clang::TypedefType>(T)) {
            qualifier(typedef_type->getQualifier());
            add(typedef_type->getDecl(), typedef_type->getDecl());
        } else if(auto* using_type = llvm::dyn_cast<clang::UsingType>(T)) {
            qualifier(using_type->getQualifier());
            add(using_type->getDecl(), using_type->getDecl()->getTargetDecl());
        } else if(auto* tag = llvm::dyn_cast<clang::TagType>(T)) {
            tag_type(tag);
        } else if(auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(T)) {
            auto name = TST->getTemplateName();
            qualifier(name.getQualifier());
            if(auto* decl = name.getAsTemplateDecl()) {
                add(decl, types::decl_of(type));
            }
            arguments(TST->template_arguments());
        } else if(auto* param = llvm::dyn_cast<clang::TemplateTypeParmType>(T)) {
            add(param->getDecl(), param->getDecl());
        } else if(auto* subst = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(T)) {
            walk(subst->getReplacementType());
        } else if(auto* deduced = llvm::dyn_cast<clang::DeducedType>(T)) {
            walk(deduced->getDeducedType());
        } else if(auto* member = llvm::dyn_cast<clang::MemberPointerType>(T)) {
            walk(member->getPointeeType());
            qualifier(member->getQualifier());
        } else if(auto* function = llvm::dyn_cast<clang::FunctionProtoType>(T)) {
            walk(function->getReturnType());
            for(auto param: function->param_types()) {
                walk(param);
            }
        } else if(auto* attributed = llvm::dyn_cast<clang::AttributedType>(T)) {
            walk(attributed->getModifiedType());
        } else if(auto* adjusted = llvm::dyn_cast<clang::AdjustedType>(T)) {
            walk(adjusted->getAdjustedType());
        } else if(auto* atomic = llvm::dyn_cast<clang::AtomicType>(T)) {
            walk(atomic->getValueType());
        } else if(auto* expansion = llvm::dyn_cast<clang::PackExpansionType>(T)) {
            walk(expansion->getPattern());
        } else if(llvm::isa<clang::PointerType, clang::ReferenceType>(T)) {
            walk(T->getPointeeType());
        } else if(auto* array = llvm::dyn_cast<clang::ArrayType>(T)) {
            walk(array->getElementType());
        } else if(llvm::isa<clang::ParenType, clang::MacroQualifiedType>(T)) {
            walk(T->getLocallyUnqualifiedSingleStepDesugaredType());
        }
    }

private:
    void tag_type(const clang::TagType* tag) {
        const auto* decl = tag->getDecl();
        // The printer names a class by its preferred name when the class
        // declares one for this very specialization (`string` for
        // `basic_string<char>`).
        for(const auto* preferred:
            decl->getMostRecentDecl()->specific_attrs<clang::PreferredNameAttr>()) {
            auto alias = preferred->getTypedefType();
            if(clang::declaresSameEntity(alias->getAsCXXRecordDecl(), decl)) {
                while(!llvm::isa<clang::TypedefType, clang::TemplateSpecializationType>(alias)) {
                    alias = alias->getLocallyUnqualifiedSingleStepDesugaredType();
                }
                walk(alias);
                return;
            }
        }
        if(!tag->isCanonicalUnqualified()) {
            qualifier(tag->getQualifier());
        }
        add(decl, decl);
        if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            if(const auto* written = spec->getTemplateArgsAsWritten()) {
                for(const auto& arg: written->arguments()) {
                    argument(arg.getArgument());
                }
            } else {
                arguments(spec->getTemplateArgs().asArray());
            }
        }
    }

    /// A qualifier's types print before the name; namespaces get no link.
    void qualifier(clang::NestedNameSpecifier qualifier) {
        if(qualifier.getKind() == clang::NestedNameSpecifier::Kind::Type) {
            walk(clang::QualType(qualifier.getAsType(), 0));
        }
    }

    /// Trailing arguments equal to their parameter's default are not
    /// printed.
    void arguments(llvm::ArrayRef<clang::TemplateArgument> args) {
        while(!args.empty() && args.back().getIsDefaulted()) {
            args = args.drop_back();
        }
        for(const auto& arg: args) {
            argument(arg);
        }
    }

    void argument(const clang::TemplateArgument& arg) {
        switch(arg.getKind()) {
            case clang::TemplateArgument::Type: walk(arg.getAsType()); break;
            case clang::TemplateArgument::Template:
            case clang::TemplateArgument::TemplateExpansion: {
                if(auto* decl = arg.getAsTemplateOrTemplatePattern().getAsTemplateDecl()) {
                    add(decl, decl);
                }
                break;
            }
            case clang::TemplateArgument::Pack: {
                for(const auto& element: arg.pack_elements()) {
                    argument(element);
                }
                break;
            }
            default: break;
        }
    }

    /// `spelled` gives the name, `target` the declaration the link reaches.
    void add(const clang::NamedDecl* spelled, const clang::NamedDecl* target) {
        const auto* record = llvm::dyn_cast_if_present<clang::CXXRecordDecl>(target);
        if(!target || !spelled->getIdentifier() || (record && record->isLambda())) {
            return;
        }
        names.push_back({spelled->getName(), target});
    }
};

/// Collects hints by walking the unit's cached Semantics node table — the
/// DFS pre-order record of the main file's written AST — instead of
/// running another RecursiveASTVisitor over the TU. The table stores only
/// structure; everything a hint needs is derived from the recorded node.
class Collector {
public:
    Collector(std::vector<InlayHint>& result,
              CompilationUnitRef unit,
              LocalSourceRange restrict_range,
              const InlayHintsOptions& options) :
        result(result), unit(unit), restrict_range(restrict_range), options(options) {}

    void run() {
        auto nodes = unit.semantics().node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];

            // The preprocessor segment follows the AST segment; directives
            // produce no hints.
            if(!entry.node.is_ast()) {
                break;
            }

            // An instantiation subtree repeats the pattern's locations;
            // hinting it duplicates every hint there — and with several
            // instantiations the deduced types contradict each other.
            if(entry.flags.in_instantiation) {
                index = entry.subtree_end;
                continue;
            }

            switch(entry.node.kind()) {
                case SemanticNode::Kind::Decl: {
                    handle_decl(entry.node.get<clang::Decl>());
                    break;
                }

                case SemanticNode::Kind::Stmt:
                    if(!handle_stmt(entry.node.get<clang::Stmt>())) {
                        index = entry.subtree_end;
                        continue;
                    }
                    break;

                case SemanticNode::Kind::TypeLoc:
                    handle_type_loc(*entry.node.get<clang::TypeLoc>());
                    break;

                default: break;
            }

            index += 1;
        }
    }

private:
    // Get the range of the main file that *exactly* corresponds to R.
    std::optional<LocalSourceRange> hint_range(clang::SourceRange R) {
        auto tokens = unit.spelled_tokens(R);

        if(tokens.empty()) {
            return std::nullopt;
        }

        auto begin = tokens.front().location();
        auto end = tokens.back().endLocation();

        auto [begin_fid, begin_offset] = unit.decompose_location(tokens.front().location());
        auto [end_fid, end_offset] = unit.decompose_location(tokens.back().endLocation());

        // Hint must be within the main file, not e.g. a non-preamble include.
        if(begin_fid != end_fid || begin_fid != unit.main_file()) {
            return std::nullopt;
        }

        return LocalSourceRange{begin_offset, end_offset};
    }

    // Compute the LSP range to attach the block end hint to, if any allowed.
    // 1. "}" is the last non-whitespace character on the line. The range of "}"
    // is returned.
    // 2. After "}", if the trimmed trailing text is exactly
    // `OptionalPunctuation`, say ";". The range of "} ... ;" is returned.
    // Otherwise, the hint shouldn't be shown.
    std::optional<LocalSourceRange> compute_block_end_range(clang::SourceRange brace_range,
                                                            llvm::StringRef optional_punctuation) {
        constexpr unsigned HintMinLineLimit = 2;

        auto [block_begin_fid, block_begin_offset] =
            unit.decompose_location(unit.file_location(brace_range.getBegin()));
        auto rbrace_loc = unit.file_location(brace_range.getEnd());
        auto [rbrace_fid, rbrace_offset] = unit.decompose_location(rbrace_loc);

        // Because we need to check the block satisfies the minimum line limit, we
        // require both source location to be in the main file. This prevents hint
        // to be shown in weird cases like '{' is actually in a "#include", but it's
        // rare anyway.
        if(block_begin_fid != rbrace_fid || block_begin_fid != unit.main_file()) {
            return std::nullopt;
        }

        llvm::StringRef rest_of_line = unit.main_content().substr(rbrace_offset).split('\n').first;
        if(!rest_of_line.starts_with("}")) {
            return std::nullopt;
        }

        llvm::StringRef trailing_text = rest_of_line.drop_front().trim();
        if(!trailing_text.empty() && trailing_text != optional_punctuation) {
            return std::nullopt;
        }

        auto& src_mgr = unit.context().getSourceManager();
        auto block_begin_line = src_mgr.getLineNumber(block_begin_fid, block_begin_offset);
        auto rbrace_line = src_mgr.getLineNumber(rbrace_fid, rbrace_offset);

        // Don't show hint on trivial blocks like `class X {};`
        if(block_begin_line + HintMinLineLimit - 1 > rbrace_line) {
            return std::nullopt;
        }

        // This is what we attach the hint to, usually "}" or "};".
        llvm::StringRef text = rest_of_line.take_front(
            trailing_text.empty() ? 1 : trailing_text.bytes_end() - rest_of_line.bytes_begin());

        /// FIXME: Handle case, if RBraceLoc is from macro expansion.
        return LocalSourceRange(rbrace_offset, rbrace_offset + text.size());
    }

    /// Check whether the expr has a param name comment before it.
    /// The typical format is `/*name=*/`.
    bool has_param_name_comment(const clang::Expr* expr, llvm::StringRef name) {
        auto location = unit.file_location(expr->getBeginLoc());
        auto [fid, offset] = unit.decompose_location(location);
        if(fid != unit.main_file()) {
            return false;
        }

        llvm::StringRef content = unit.main_content().substr(0, offset);

        // Allow whitespace between comment and expression.
        content = content.rtrim();
        if(!content.consume_back("*/")) {
            return false;
        }

        // Ignore some punctuation and whitespace around comment.
        // In particular this allows designators to match nicely.
        llvm::StringLiteral ignore_chars = " =.";
        name = name.trim(ignore_chars);
        content = content.rtrim(ignore_chars);

        // Other than that, the comment must contain exactly name.
        if(!content.consume_back(name)) {
            return false;
        }

        content = content.rtrim(ignore_chars);
        return content.ends_with("/*");
    }

    bool should_hint_name(const clang::Expr* expr, llvm::StringRef name) {
        if(name.empty())
            return false;

        // If the argument expression is a single name and it matches the
        // parameter name exactly, omit the name hint.
        if(name == display::identifier_of(expr))
            return false;

        // Exclude argument expressions preceded by a /*paramName*/.
        if(has_param_name_comment(expr, name)) {
            return false;
        }

        return true;
    }

    using NameVec = llvm::SmallVector<llvm::StringRef, 8>;

    /// Each parameter's name, and the declaration spelling it.
    NameVec choose_param_names(llvm::ArrayRef<const clang::ParmVarDecl*> params,
                               llvm::SmallVectorImpl<const clang::ParmVarDecl*>& named) {
        NameVec param_names;
        for(const auto* param: params) {
            if(decls::underlying_pack_type(param)) {
                // If we haven't resolved a pack paramater (e.g. foo(Args... args)) to a
                // non-pack parameter, then hinting as foo(args: 1, args: 2, args: 3) is
                // unlikely to be useful.
                param_names.emplace_back();
            } else {
                llvm::StringRef simple_name = display::identifier_of(param);
                // If the parameter is unnamed in the declaration:
                // attempt to get its name from the definition
                if(simple_name.empty()) {
                    if(const auto* def = param_definition(param)) {
                        simple_name = display::identifier_of(def);
                        param = def;
                    }
                }
                // Still unnamed: an empty entry, the hint is dropped later.
                param_names.emplace_back(simple_name);
            }
            named.push_back(param);
        }

        // Standard library functions often have parameter names that start
        // with underscores, which makes the hints noisy, so strip them out.
        for(auto& name: param_names) {
            name = name.ltrim('_');
        }

        return param_names;
    }

    void add_params(Callee callee,
                    clang::SourceLocation rpunc_location,
                    llvm::ArrayRef<const clang::Expr*> args) {
        assert(callee.decl || callee.loc);

        if((!options.parameters && !options.default_arguments) || args.size() == 0) {
            return;
        }

        if(callee.decl) {
            /// We don't want to hint for copy or move constructors, which may make
            /// a lot of noise.
            auto ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(callee.decl);
            if(ctor && ctor->isCopyOrMoveConstructor()) {
                return;
            }
        }

        llvm::SmallVector<std::vector<InlayHintPart>> formatted_default_args;
        bool has_non_default_args = false;

        llvm::ArrayRef<const clang::ParmVarDecl*> params, forwarded_params;
        // Resolve parameter packs to their forwarded parameter
        llvm::SmallVector<const clang::ParmVarDecl*> forwarded_params_storage;

        auto remove_self_params = [](llvm::ArrayRef<const clang::ParmVarDecl*> params)
            -> llvm::ArrayRef<const clang::ParmVarDecl*> {
            if(!params.empty() && params.front()->isExplicitObjectParameter()) {
                params = params.drop_front(1);
            }
            return params;
        };

        if(callee.decl) {
            params = remove_self_params(callee.decl->parameters());
            forwarded_params_storage = decls::resolve_forwarding_params(callee.decl);
            forwarded_params = remove_self_params(forwarded_params_storage);
        } else {
            params = remove_self_params(callee.loc.getParams());
            forwarded_params = {params.begin(), params.end()};
        }

        llvm::SmallVector<const clang::ParmVarDecl*, 8> named_params;
        NameVec param_names = choose_param_names(forwarded_params, named_params);

        // Exclude setters (i.e. functions with one argument whose name begins with
        // "set"), and builtins like std::move/forward/... as their parameter name
        // is also not likely to be interesting.
        if(callee.decl && (is_setter(callee.decl, param_names) || is_simple_builtin(callee.decl)))
            return;

        for(size_t i = 0; i < param_names.size() && i < args.size(); ++i) {
            // Pack expansion expressions cause the 1:1 mapping between arguments and
            // parameters to break down, so we don't add further inlay hints if we
            // encounter one.
            if(llvm::isa<clang::PackExpansionExpr>(args[i])) {
                break;
            }

            llvm::StringRef name = param_names[i];
            const bool name_hint = should_hint_name(args[i], name) && options.parameters;
            const bool reference_hint =
                decls::binds_mutable_reference(params[i], forwarded_params[i]) &&
                options.parameters;

            const bool is_default = llvm::isa<clang::CXXDefaultArgExpr>(args[i]);
            has_non_default_args |= !is_default;
            if(is_default) {
                if(options.default_arguments) {
                    const auto text = clang::Lexer::getSourceText(
                        clang::CharSourceRange::getTokenRange(params[i]->getDefaultArgRange()),
                        unit.context().getSourceManager(),
                        unit.lang_options());
                    // type_name_limit = 0 means unlimited, as for type hints.
                    const bool too_long =
                        options.type_name_limit && text.size() > options.type_name_limit;
                    const auto abbrev = (too_long || text.contains("\n")) ? "..." : text;
                    auto& element = formatted_default_args.emplace_back();
                    if(name_hint) {
                        append(element, linked(name, named_params[i]));
                        append(element, plain(": "));
                    }
                    append(element, plain(abbrev));
                }
            } else if(name_hint || reference_hint) {
                std::vector<InlayHintPart> label;
                if(name_hint) {
                    label.push_back(linked(name, named_params[i]));
                }
                add_inlay_hint(args[i]->getSourceRange(),
                               HintSide::Left,
                               HintCategory::Parameter,
                               reference_hint ? "&" : "",
                               std::move(label),
                               ": ");
            }
        }

        if(!formatted_default_args.empty()) {
            std::vector<InlayHintPart> hint;
            std::size_t size = 0;
            for(auto&& element: formatted_default_args) {
                if(!hint.empty()) {
                    append(hint, plain(", "));
                    size += 2;
                }
                auto element_size = text_size(element);
                if(options.type_name_limit && size + element_size >= options.type_name_limit) {
                    append(hint, plain("..."));
                    break;
                }
                for(auto& part: element) {
                    append(hint, std::move(part));
                }
                size += element_size;
            }

            add_inlay_hint(clang::SourceRange(rpunc_location),
                           HintSide::Left,
                           HintCategory::DefaultArgument,
                           has_non_default_args ? ", " : "",
                           std::move(hint),
                           "");
        }
    }

    void add_block_end_hint(clang::SourceRange brace_range,
                            llvm::StringRef decl_prefix,
                            llvm::StringRef name,
                            llvm::StringRef optional_punctuation) {
        auto hint_range = compute_block_end_range(brace_range, optional_punctuation);
        if(!hint_range)
            return;

        std::string label = decl_prefix.str();
        if(!label.empty() && !name.empty()) {
            label += ' ';
        }
        label += name;

        constexpr unsigned HintMaxLengthLimit = 60;
        if(label.length() > HintMaxLengthLimit) {
            return;
        }

        add_inlay_hint(*hint_range,
                       HintSide::Right,
                       HintCategory::BlockEnd,
                       " // ",
                       {plain(label)},
                       "");
    }

    void mark_block_end(const clang::Stmt* body, llvm::StringRef label, llvm::StringRef name = "") {
        if(const auto* cs = llvm::dyn_cast_or_null<clang::CompoundStmt>(body)) {
            add_block_end_hint(cs->getSourceRange(), label, name, "");
        }
    }

    static InlayHintPart plain(llvm::StringRef text) {
        return {.value = text.str()};
    }

    /// A label piece reaching `decl`, as the index knows it.
    InlayHintPart linked(llvm::StringRef text, const clang::NamedDecl* decl) {
        // Only a written declaration has rows to reach.
        if(decl->getLocation().isInvalid()) {
            return plain(text);
        }
        if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(decl)) {
            decl = written_param(param);
        }
        const auto* symbol = decls::normalize(decl);
        InlayHintPart part{.value = text.str(), .symbol = unit.entity(symbol)};
        if(index::classify_scope(symbol) != index::SymbolScope::External) {
            part.anchor = unit.file_path(unit.file_id(unit.file_location(symbol->getLocation())));
        }
        return part;
    }

    /// The parameter of the written function an instantiated one stands
    /// for: an instantiation's declarations have no rows of their own.
    const static clang::ParmVarDecl* written_param(const clang::ParmVarDecl* param) {
        const auto* function = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
        const auto* pattern = function ? function->getTemplateInstantiationPattern() : nullptr;
        if(!pattern) {
            return param;
        }
        // A pack expands to any number of parameters: past it, indices
        // part ways.
        auto index = param->getFunctionScopeIndex();
        auto patterns = pattern->parameters().take_front(index + 1);
        if(patterns.size() <= index ||
           llvm::any_of(patterns, [](const auto* each) { return each->isParameterPack(); })) {
            return param;
        }
        return patterns.back();
    }

    /// Append a piece, merging plain text into the plain text before it.
    static void append(std::vector<InlayHintPart>& label, InlayHintPart part) {
        if(part.value.empty()) {
            return;
        }
        if(!label.empty() && !label.back().symbol && !part.symbol) {
            label.back().value += part.value;
            return;
        }
        label.push_back(std::move(part));
    }

    static std::size_t text_size(llvm::ArrayRef<InlayHintPart> label) {
        std::size_t size = 0;
        for(const auto& part: label) {
            size += part.value.size();
        }
        return size;
    }

    /// The pieces of `text`, the printed form of `type`: a name the printer
    /// wrote for a declaration links to it, the rest stays plain.
    std::vector<InlayHintPart> type_label(clang::QualType type, llvm::StringRef text) {
        SpelledNames spelled;
        spelled.walk(type);
        std::vector<InlayHintPart> label;
        std::size_t cursor = 0;
        for(const auto& [name, decl]: spelled.names) {
            for(auto at = text.find(name, cursor); at != llvm::StringRef::npos;
                at = text.find(name, at + 1)) {
                auto end = at + name.size();
                bool whole = (at == 0 || !clang::isAsciiIdentifierContinue(text[at - 1])) &&
                             (end == text.size() || !clang::isAsciiIdentifierContinue(text[end]));
                if(whole) {
                    append(label, plain(text.slice(cursor, at)));
                    append(label, linked(name, decl));
                    cursor = end;
                    break;
                }
            }
        }
        append(label, plain(text.substr(cursor)));
        return label;
    }

    // We pass HintSide rather than SourceLocation because we want to ensure
    // it is in the same file as the common file range.
    void add_inlay_hint(clang::SourceRange range,
                        HintSide side,
                        HintCategory kind,
                        llvm::StringRef prefix,
                        std::vector<InlayHintPart> label,
                        llvm::StringRef suffix) {
        auto local_range = hint_range(range);
        if(!local_range) {
            return;
        }

        add_inlay_hint(*local_range, side, kind, prefix, std::move(label), suffix);
    }

    void add_inlay_hint(LocalSourceRange range,
                        HintSide side,
                        HintCategory kind,
                        llvm::StringRef prefix,
                        std::vector<InlayHintPart> label,
                        llvm::StringRef suffix) {
        // We shouldn't get as far as adding a hint if the category is disabled.
        // We'd like to disable as much of the analysis as possible above instead.
        // Assert in debug mode but add a dynamic check in production.
        assert(options.enabled && "Shouldn't get here if disabled!");

        std::uint32_t offset = side == HintSide::Left ? range.begin : range.end;
        if(restrict_range.valid() && !restrict_range.contains(offset))
            return;

        bool pad_left = prefix.consume_front(" ");
        bool pad_right = suffix.consume_back(" ");

        InlayHint hint{
            .offset = offset,
            .kind = kind,
            .padding_left = pad_left,
            .padding_right = pad_right,
        };
        append(hint.label, plain(prefix));
        for(auto& part: label) {
            append(hint.label, std::move(part));
        }
        append(hint.label, plain(suffix));
        result.push_back(std::move(hint));
    }

    void add_type_hint(clang::SourceRange range, clang::QualType type, llvm::StringRef prefix) {
        if(!options.deduced_types || type.isNull())
            return;

        auto printed = display::maybe_desugar(unit.context(), type);
        std::string type_name = display::type(unit.context(), printed, display_options).text;

        auto should_print = [&](llvm::StringRef TypeName) {
            return options.type_name_limit == 0 || TypeName.size() < options.type_name_limit;
        };

        if(type != printed && !should_print(type_name)) {
            // If the desugared type is too long to display, fallback to the sugared
            // type.
            printed = type;
            type_name = display::type(unit.context(), type, display_options).text;
        }

        if(should_print(type_name)) {
            add_inlay_hint(range,
                           HintSide::Right,
                           HintCategory::Type,
                           prefix,
                           type_label(printed, type_name),
                           /*Suffix=*/"");
        }
    }

    /// Hint the unwritten designators of a syntactic init list, e.g. `.x`
    /// for the `1` in `Point{1}`. Explicitly written designators and inits
    /// already carrying a `/*name=*/` comment stay bare.
    void add_designators(const clang::InitListExpr* syntactic) {
        auto designators = clang::tidy::utils::getUnwrittenDesignators(syntactic);
        for(const clang::Expr* init: syntactic->inits()) {
            if(llvm::isa<clang::DesignatedInitExpr>(init)) {
                continue;
            }
            auto it = designators.find(init->getBeginLoc());
            if(it == designators.end() || has_param_name_comment(init, it->second)) {
                continue;
            }
            add_inlay_hint(init->getSourceRange(),
                           HintSide::Left,
                           HintCategory::Designator,
                           /*Prefix=*/"",
                           designator_label(syntactic->getType(), it->second),
                           /*Suffix=*/"=");
        }
    }

    /// The pieces of a designator such as `.inner.x` or `[2].x`: each field
    /// name links to the field, looked up from the aggregate's type down.
    std::vector<InlayHintPart> designator_label(clang::QualType aggregate,
                                                llvm::StringRef designator) {
        std::vector<InlayHintPart> label;
        while(!designator.empty()) {
            if(designator.consume_front("[")) {
                auto index = designator.take_until([](char c) { return c == ']'; });
                designator = designator.drop_front(index.size() + 1);
                append(label, plain(("[" + index + "]").str()));
                const auto* array = aggregate->getAsArrayTypeUnsafe();
                aggregate = array ? array->getElementType() : clang::QualType();
                continue;
            }
            designator.consume_front(".");
            auto name = designator.take_until([](char c) { return c == '.' || c == '['; });
            designator = designator.drop_front(name.size());
            append(label, plain("."));
            const auto* record = aggregate.isNull() ? nullptr : aggregate->getAsRecordDecl();
            const clang::ValueDecl* field = nullptr;
            if(record) {
                auto& identifier = unit.context().Idents.get(name);
                for(auto* found: record->lookup(&identifier)) {
                    if(auto* indirect = llvm::dyn_cast<clang::IndirectFieldDecl>(found)) {
                        field = indirect->getAnonField();
                    } else if(auto* direct = llvm::dyn_cast<clang::FieldDecl>(found)) {
                        field = direct;
                    }
                }
            }
            append(label, field ? linked(name, field) : plain(name));
            aggregate = field ? field->getType() : clang::QualType();
        }
        return label;
    }

    void add_return_type_hint(const clang::FunctionDecl* decl, clang::SourceRange range) {
        auto* type = decl->getReturnType()->getContainedAutoType();
        if(!type || type->getDeducedType().isNull()) {
            return;
        }
        add_type_hint(range, decl->getReturnType(), /*Prefix=*/"-> ");
    }

    // A PseudoObjectExpr typically incorporates a syntactic expression and
    // several semantic expressions; the table records only the syntactic
    // subtree. Returns false to skip that subtree.
    bool handle_pseudo_object(const clang::PseudoObjectExpr* expr) {
        const clang::Expr* syntactic_expr = expr->getSyntacticForm();
        if(llvm::isa<clang::CallExpr>(syntactic_expr)) {
            // Since the counterpart semantics usually get the identical source
            // locations as the syntactic one, visiting those would end up presenting
            // confusing hints e.g., __builtin_dump_struct.
            // Thus, only visit the syntactic forms if this is written as a
            // CallExpr. This leaves the door open in case the arguments in the
            // syntactic form could possibly get parameter names.
            return true;
        }

        // We don't want the hints for some of the MS property extensions.
        // e.g.
        // struct S {
        //   __declspec(property(get=GetX, put=PutX)) int x[];
        //   void PutX(int y);
        //   void Work(int y) { x = y; } // Bad: `x = y: y`.
        // };
        if(llvm::isa<clang::BinaryOperator>(syntactic_expr)) {
            return false;
        }

        // Other forms (an MS property subscript read `s.x[1][2]`) execute an
        // accessor call that exists only in the semantic expressions, so its
        // parameter hints are derived here. The written arguments appear in
        // both forms as the same OpaqueValueExprs, so everything below the
        // call is already covered by the recorded syntactic subtree.
        for(const clang::Expr* semantic_expr: expr->semantics()) {
            if(auto* call = dyn_cast<clang::CallExpr>(semantic_expr)) {
                handle_call(call);
            }
        }
        return true;
    }

    void handle_decl(const clang::Decl* decl) {
        if(auto* ns = dyn_cast<clang::NamespaceDecl>(decl)) {
            handle_namespace(ns);
        } else if(auto* tag = dyn_cast<clang::TagDecl>(decl)) {
            handle_tag(tag);
        } else if(auto* function = dyn_cast<clang::FunctionDecl>(decl)) {
            handle_function(function);
        } else if(auto* var = dyn_cast<clang::VarDecl>(decl)) {
            handle_var(var);
        }
    }

    /// Returns false to skip the node's recorded subtree.
    bool handle_stmt(const clang::Stmt* stmt) {
        if(auto* expr = dyn_cast<clang::PseudoObjectExpr>(stmt)) {
            return handle_pseudo_object(expr);
        }

        if(auto* expr = dyn_cast<clang::CallExpr>(stmt)) {
            handle_call(expr);
        } else if(auto* expr = dyn_cast<clang::CXXConstructExpr>(stmt)) {
            handle_construct(expr);
        } else if(auto* expr = dyn_cast<clang::LambdaExpr>(stmt)) {
            handle_lambda(expr);
        } else if(auto* expr = dyn_cast<clang::InitListExpr>(stmt)) {
            handle_init_list(expr);
        } else if(auto* for_stmt = dyn_cast<clang::ForStmt>(stmt)) {
            handle_for(for_stmt);
        } else if(auto* range_for = dyn_cast<clang::CXXForRangeStmt>(stmt)) {
            handle_range_for(range_for);
        } else if(auto* while_stmt = dyn_cast<clang::WhileStmt>(stmt)) {
            handle_while(while_stmt);
        } else if(auto* switch_stmt = dyn_cast<clang::SwitchStmt>(stmt)) {
            handle_switch(switch_stmt);
        } else if(auto* if_stmt = dyn_cast<clang::IfStmt>(stmt)) {
            handle_if(if_stmt);
        }
        return true;
    }

    void handle_namespace(const clang::NamespaceDecl* decl) {
        if(options.block_end) {
            // For namespace, the range actually starts at the namespace keyword. But
            // it should be fine since it's usually very short.
            // Anonymous namespaces hint as plain "// namespace".
            add_block_end_hint(decl->getSourceRange(),
                               "namespace",
                               display::identifier_of(decl),
                               "");
        }
    }

    void handle_tag(const clang::TagDecl* decl) {
        if(options.block_end && decl->isThisDeclarationADefinition()) {
            std::string prefix = decl->getKindName().str();

            if(const auto* enum_decl = dyn_cast<clang::EnumDecl>(decl)) {
                if(enum_decl->isScoped()) {
                    prefix += enum_decl->isScopedUsingClassTag() ? " class" : " struct";
                }
            };

            // Anonymous tags hint with the bare kind, e.g. "// struct".
            add_block_end_hint(decl->getBraceRange(), prefix, display::identifier_of(decl), ";");
        }
    }

    void handle_function(const clang::FunctionDecl* decl) {
        if(auto* proto_type = llvm::dyn_cast<clang::FunctionProtoType>(decl->getType())) {
            if(!proto_type->hasTrailingReturn()) {
                if(auto FTL = decl->getFunctionTypeLoc()) {
                    add_return_type_hint(decl, FTL.getRParenLoc());
                }
            }
        }

        if(options.block_end && decl->isThisDeclarationADefinition()) {
            // We use `printName` here to properly print name of ctor/dtor/operator
            // overload.
            if(const clang::Stmt* body = decl->getBody()) {
                add_block_end_hint(body->getSourceRange(), "", display::name_of(decl), "");
            }
        }
    }

    void handle_var(const clang::VarDecl* var) {
        // Do not show hints for the aggregate in a structured binding,
        // but show hints for the individual bindings.
        if(auto* decl = dyn_cast<clang::DecompositionDecl>(var)) {
            for(auto* binding: decl->bindings()) {
                // For structured bindings, print canonical types. This is important
                // because for bindings that use the tuple_element protocol, the
                // non-canonical types would be "tuple_element<I, A>::type".
                if(auto type = binding->getType(); !type.isNull() && !type->isDependentType()) {
                    add_type_hint(binding->getLocation(),
                                  type.getCanonicalType(),
                                  /*Prefix=*/": ");
                }
            }
            return;
        }

        auto type = var->getType();
        if(auto* auto_type = type->getContainedAutoType()) {
            if(auto_type->isDeduced() && !type->isDependentType()) {
                // Our current approach is to place the hint on the variable
                // and accordingly print the full type
                // (e.g. for `const auto& x = 42`, print `const int&`).
                // Alternatively, we could place the hint on the `auto`
                // (and then just print the type deduced for the `auto`).
                add_type_hint(var->getLocation(), var->getType(), /*Prefix=*/": ");
            }
        }

        // Handle templates like `int foo(auto x)` with exactly one instantiation.
        if(auto* param = llvm::dyn_cast<clang::ParmVarDecl>(var)) {
            if(var->getIdentifier() && type->isDependentType()) {
                auto unwrapped = types::unwrap(var->getTypeSourceInfo()->getTypeLoc());
                if(auto type = unwrapped.getAs<clang::TemplateTypeParmTypeLoc>()) {
                    if(auto decl = type.getDecl(); decl && decl->isImplicit()) {
                        // The helper keeps its clangd signature, which speaks
                        // mutable pointers; it only reads through them.
                        if(auto* IPVD =
                               decls::only_instantiation(const_cast<clang::ParmVarDecl*>(param))) {
                            add_type_hint(var->getLocation(),
                                          IPVD->getType(),
                                          /*Prefix=*/": ");
                        }
                    }
                }
            }
        }
    }

    void handle_construct(const clang::CXXConstructExpr* expr) {
        // Weed out constructor calls that don't look like a function call with
        // an argument list, by checking the validity of getParenOrBraceRange().
        // Also weed out std::initializer_list constructors as there are no names
        // for the individual arguments.
        if(!expr->getParenOrBraceRange().isValid() || expr->isStdInitListInitialization()) {
            return;
        }

        Callee callee;
        callee.decl = expr->getConstructor();
        if(!callee.decl) {
            return;
        }

        add_params(callee,
                   expr->getParenOrBraceRange().getEnd(),
                   {expr->getArgs(), expr->getNumArgs()});
    }

    void handle_call(const clang::CallExpr* expr) {
        if(!options.parameters && !options.default_arguments) {
            return;
        }

        auto isFunctionObjectCallExpr = [](const clang::CallExpr* E) {
            if(auto* call_expr = dyn_cast<clang::CXXOperatorCallExpr>(E)) {
                return call_expr->getOperator() == clang::OverloadedOperatorKind::OO_Call;
            }

            return false;
        };

        bool is_functor = isFunctionObjectCallExpr(expr);

        // Do not show parameter hints for user-defined literals or
        // operator calls except for operator(). (Among other reasons, the resulting
        // hints can look awkward, e.g. the expression can itself be a function
        // argument and then we'd get two hints side by side).
        if((llvm::isa<clang::CXXOperatorCallExpr>(expr) && !is_functor) ||
           llvm::isa<clang::UserDefinedLiteral>(expr))
            return;

        Callee callee;
        bool dependent_callee = false;
        if(expr->isTypeDependent() || expr->isValueDependent()) {
            dependent_callee = true;
            // A dependent call has no resolved callee. The template
            // resolver's arity-filtered candidate set stands in; only a
            // unique candidate gives trustworthy parameter names — with
            // several overloads left we must not pick one arbitrarily.
            auto candidates = unit.resolver().lookup(expr);
            if(candidates.size() != 1) {
                return;
            }

            auto* target = candidates.front();
            if(auto* shadow = dyn_cast<clang::UsingShadowDecl>(target)) {
                target = shadow->getTargetDecl();
            }
            if(auto* FTD = dyn_cast<clang::FunctionTemplateDecl>(target)) {
                callee.decl = FTD->getTemplatedDecl();
            } else {
                callee.decl = dyn_cast<clang::FunctionDecl>(target);
            }
            if(!callee.decl) {
                return;
            }
        } else if(const auto* FD = dyn_cast_or_null<clang::FunctionDecl>(expr->getCalleeDecl())) {
            callee.decl = FD;
        } else if(const auto* FTD =
                      dyn_cast_or_null<clang::FunctionTemplateDecl>(expr->getCalleeDecl())) {
            callee.decl = FTD->getTemplatedDecl();
        } else if(clang::FunctionProtoTypeLoc loc =
                      // The helper keeps its clangd signature, which speaks
                      // mutable pointers; it only reads through them.
                  decls::proto_type_loc(const_cast<clang::Expr*>(expr->getCallee()))) {
            callee.loc = loc;
        } else {
            return;
        }

        // N4868 [over.call.object]p3 says,
        // The argument list submitted to overload resolution consists of the
        // argument expressions present in the function call syntax preceded by the
        // implied object argument (E).
        //
        // As well as the provision from P0847R7 Deducing This [expr.call]p7:
        // ...If the function is an explicit object member function and there is an
        // implied object argument ([over.call.func]), the list of provided
        // arguments is preceded by the implied object argument for the purposes of
        // this correspondence...
        llvm::ArrayRef<const clang::Expr*> args = {expr->getArgs(), expr->getNumArgs()};

        // We don't have the implied object argument through a function pointer
        // either, nor in a dependent member call: there the object stays the
        // member expression's base and never joins the argument list.
        if(const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee.decl)) {
            if(is_functor ||
               (!dependent_callee && method->hasCXXExplicitFunctionObjectParameter())) {
                args = args.drop_front(1);
            }
        }

        add_params(callee, expr->getRParenLoc(), args);
    }

    void handle_for(const clang::ForStmt* S) {
        if(options.block_end) {
            std::string name;
            // Common case: for (int I = 0; I < N; I++). Use "I" as the name.
            if(auto* DS = llvm::dyn_cast_or_null<clang::DeclStmt>(S->getInit());
               DS && DS->isSingleDecl()) {
                name = display::identifier_of(llvm::cast<clang::NamedDecl>(DS->getSingleDecl()));
            } else if(const auto* cond = S->getCond()) {
                name = display::summarize(cond);
            }
            mark_block_end(S->getBody(), "for", name);
        }
    }

    void handle_range_for(const clang::CXXForRangeStmt* S) {
        if(options.block_end) {
            // Decomposition loop variables have no identifier: plain "// for".
            mark_block_end(S->getBody(), "for", display::identifier_of(S->getLoopVariable()));
        }
    }

    void handle_while(const clang::WhileStmt* S) {
        if(options.block_end) {
            mark_block_end(S->getBody(), "while", display::summarize(S->getCond()));
        }
    }

    void handle_switch(const clang::SwitchStmt* S) {
        if(options.block_end) {
            mark_block_end(S->getBody(), "switch", display::summarize(S->getCond()));
        }
    }

    void handle_if(const clang::IfStmt* S) {
        if(options.block_end) {
            // Pre-order guarantees the outer if is handled before its else-if.
            if(const auto* else_if = llvm::dyn_cast_or_null<clang::IfStmt>(S->getElse())) {
                else_ifs.insert(else_if);
            }

            // Don't use markBlockEnd: the relevant range is [then.begin, else.end].
            if(const auto* if_end = llvm::dyn_cast<clang::CompoundStmt>(
                   S->getElse() ? S->getElse() : S->getThen())) {
                std::string name;
                // `if consteval` has no condition to summarize.
                if(const auto* cond = S->getCond(); cond && !else_ifs.contains(S)) {
                    name = display::summarize(cond);
                }
                add_block_end_hint({S->getThen()->getBeginLoc(), if_end->getRBracLoc()},
                                   "if",
                                   name,
                                   "");
            }
        }
    }

    void handle_lambda(const clang::LambdaExpr* expr) {
        const clang::FunctionDecl* decl = expr->getCallOperator();
        if(!expr->hasExplicitResultType()) {
            clang::SourceLocation type_hint_loc;
            if(!expr->hasExplicitParameters()) {
                type_hint_loc = expr->getIntroducerRange().getEnd();
            } else if(auto FTL = decl->getFunctionTypeLoc()) {
                type_hint_loc = FTL.getRParenLoc();
            }

            if(type_hint_loc.isValid()) {
                add_return_type_hint(decl, type_hint_loc);
            }
        }
    }

    void handle_init_list(const clang::InitListExpr* expr) {
        // The table records the form the enclosing AST node stores, which for
        // the outermost list is the *semantic* form. Designators attach to the
        // syntactic form — the written one, which may have subobject
        // initializers inlined without braces where the semantic form has
        // nested init-lists. getUnwrittenDesignators will look at the semantic
        // form to determine the labels.
        if(const auto* syntactic = expr->getSyntacticForm()) {
            expr = syntactic;
        }

        if(!options.designators) {
            return;
        }

        if(expr->isIdiomaticZeroInitializer(unit.lang_options())) {
            return;
        }

        add_designators(expr);
    }

    void handle_type_loc(clang::TypeLoc TL) {
        if(const auto* DT = llvm::dyn_cast<clang::DecltypeType>(TL.getType()))
            if(clang::QualType UT = DT->getUnderlyingType(); !UT->isDependentType())
                add_type_hint(TL.getSourceRange(), UT, ": ");
    }

    // FIXME: Handle RecoveryExpr to try to hint some invalid calls.

private:
    std::vector<InlayHint>& result;
    CompilationUnitRef unit;
    LocalSourceRange restrict_range;
    const InlayHintsOptions& options;

    // The sugared type is more useful in some cases, and the canonical
    // type in other cases. Suppress scopes to keep type names short;
    // anonymous tag locations stay off so lambda locations don't print.
    // Not setting PrintCanonicalTypes for "auto" allows
    // SuppressDefaultTemplateArgs (set by default) to have an effect.
    display::Options display_options = {.suppress_scope = true};

    // If/else chains are tricky.
    //   if (cond1) {
    //   } else if (cond2) {
    //   } // mark as "cond1" or "cond2"?
    // For now, the answer is neither, just mark as "if".
    // The ElseIf is a different IfStmt that doesn't know about the outer one.
    llvm::DenseSet<const clang::IfStmt*> else_ifs;  // not eligible for names
};

}  // namespace

auto inlay_hints(CompilationUnitRef unit, LocalSourceRange target, const InlayHintsOptions& options)
    -> std::vector<InlayHint> {
    if(!options.enabled) {
        return {};
    }

    std::vector<InlayHint> raw_hints;

    Collector collector(raw_hints, unit, target, options);
    collector.run();

    std::ranges::sort(raw_hints, [](const InlayHint& lhs, const InlayHint& rhs) {
        return std::tie(lhs.offset, lhs.label, lhs.kind, lhs.padding_left, lhs.padding_right) <
               std::tie(rhs.offset, rhs.label, rhs.kind, rhs.padding_left, rhs.padding_right);
    });
    auto unique_begin =
        std::ranges::unique(raw_hints, [](const InlayHint& lhs, const InlayHint& rhs) {
            return lhs.offset == rhs.offset && lhs.kind == rhs.kind && lhs.label == rhs.label &&
                   lhs.padding_left == rhs.padding_left && lhs.padding_right == rhs.padding_right;
        });
    raw_hints.erase(unique_begin.begin(), unique_begin.end());

    return raw_hints;
}

auto inlay_hints_to_protocol(llvm::ArrayRef<InlayHint> hints,
                             const PositionMap& map,
                             llvm::function_ref<void(const InlayHint&, protocol::InlayHint&)> link)
    -> std::vector<protocol::InlayHint> {
    std::vector<protocol::InlayHint> result;
    result.reserve(hints.size());

    for(const auto& hint: hints) {
        auto pos = map.to_position(hint.offset);
        if(!pos)
            continue;
        protocol::InlayHint out{.position = *pos};

        bool linked =
            llvm::any_of(hint.label, [](const InlayHintPart& part) { return part.symbol != 0; });
        if(linked && link) {
            std::vector<protocol::InlayHintLabelPart> parts;
            for(const auto& part: hint.label) {
                parts.push_back({.value = part.value});
            }
            out.label = std::move(parts);
            link(hint, out);
        } else {
            std::string label;
            for(const auto& part: hint.label) {
                label += part.value;
            }
            out.label = std::move(label);
        }

        switch(hint.kind) {
            case HintCategory::Parameter:
            case HintCategory::DefaultArgument:
                out.kind = protocol::InlayHintKind::Parameter;
                break;

            case HintCategory::Type:
            case HintCategory::Designator:
            case HintCategory::BlockEnd: out.kind = protocol::InlayHintKind::Type; break;
        }

        if(hint.padding_left) {
            out.padding_left = true;
        }
        if(hint.padding_right) {
            out.padding_right = true;
        }

        result.push_back(std::move(out));
    }

    return result;
}

}  // namespace clice::feature
