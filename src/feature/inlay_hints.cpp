module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :index.tu_index;
import :index.types;
import :semantic.decls;
import :semantic.designators;
import :semantic.display;
import :semantic.identity;
import :semantic.resolver;
import :semantic.semantics;
import :semantic.types;
import :syntax.lexical_scan;

namespace clice::feature {

namespace {

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

// For now, inlay hints are always anchored at the left or right of their range.
enum class HintSide : std::uint8_t { Left, Right };

/// The types the code wrote for the parts of a type: every alias and
/// template specialization in it, through the aliases' own definitions,
/// keyed by the canonical type each names. Clang substitutes a class
/// template's members with its canonical arguments, so the `first` of a
/// `std::map<std::string, Point>` element is a `const basic_string<char>`;
/// the map as written gives the `std::string` back.
class WrittenTypes {
public:
    explicit WrittenTypes(clang::ASTContext& context) : context(context) {}

    void collect(clang::QualType written) {
        /// The sugar of one type all names one canonical type: the code
        /// wrote the outermost, an alias's definition the ones beneath.
        bool named = false;
        while(!written.isNull()) {
            const auto* type = written.getTypePtr();
            if(llvm::isa<clang::PointerType, clang::ReferenceType>(type)) {
                written = type->getPointeeType();
                named = false;
                continue;
            }
            if(!named && llvm::isa<clang::TypedefType, clang::TemplateSpecializationType>(type)) {
                add(written);
                named = true;
            }
            if(const auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(type)) {
                for(const auto& argument: TST->template_arguments()) {
                    if(argument.getKind() == clang::TemplateArgument::Type) {
                        collect(argument.getAsType());
                    }
                }
                written = TST->isTypeAlias() ? TST->getAliasedType() : clang::QualType();
                continue;
            }
            auto desugared = written.getSingleStepDesugaredType(context);
            if(desugared == written) {
                /// Writing the type without an alias is one more way of
                /// writing it.
                if(!named) {
                    add(written);
                }
                break;
            }
            written = desugared;
        }
    }

    /// `type` with each part clang left canonical as the code wrote it, when
    /// the code wrote it one way only.
    clang::QualType as_written(clang::QualType type) const {
        auto qualifiers = type.getLocalQualifiers();
        const auto* bare = type.getTypePtr();
        while(const auto* subst = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(bare)) {
            qualifiers.addQualifiers(subst->getReplacementType().getLocalQualifiers());
            bare = subst->getReplacementType().getTypePtr();
        }
        if(!bare->isCanonicalUnqualified()) {
            return type;
        }
        if(auto it = types.find(bare); it != types.end() && !it->second.isNull()) {
            return context.getQualifiedType(it->second, qualifiers);
        }
        if(const auto* pointer = llvm::dyn_cast<clang::PointerType>(bare)) {
            return context.getQualifiedType(
                context.getPointerType(as_written(pointer->getPointeeType())),
                qualifiers);
        }
        if(const auto* reference = llvm::dyn_cast<clang::LValueReferenceType>(bare)) {
            return context.getQualifiedType(
                context.getLValueReferenceType(as_written(reference->getPointeeType())),
                qualifiers);
        }
        if(const auto* reference = llvm::dyn_cast<clang::RValueReferenceType>(bare)) {
            return context.getQualifiedType(
                context.getRValueReferenceType(as_written(reference->getPointeeType())),
                qualifiers);
        }
        return type;
    }

private:
    /// Two ways the code wrote one type leave it as clang has it: nothing
    /// says which one the code meant.
    void add(clang::QualType type) {
        auto unqualified = type.getLocalUnqualifiedType();
        auto [it, inserted] =
            types.try_emplace(context.getCanonicalType(unqualified).getTypePtr(), unqualified);
        if(!inserted && !it->second.isNull() &&
           it->second.getAsString() != unqualified.getAsString()) {
            it->second = clang::QualType();
        }
    }

    clang::ASTContext& context;
    llvm::DenseMap<const clang::Type*, clang::QualType> types;
};

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

/// Whether `c` continues a UTF-8 character rather than starting one.
bool continues_character(char c) {
    return (c & 0xC0) == 0x80;
}

/// The length of `text` in characters.
std::size_t characters(llvm::StringRef text) {
    return llvm::count_if(text, [](char c) { return !continues_character(c); });
}

/// `text` within `limit` characters, cut with `…` where it no longer fits;
/// `0` means no limit.
std::string truncate(llvm::StringRef text, std::size_t limit) {
    if(limit == 0 || characters(text) <= limit) {
        return text.str();
    }
    std::size_t kept = 0;
    std::size_t end = 0;
    for(; end < text.size(); end += 1) {
        if(!continues_character(text[end])) {
            if(kept + 1 == limit) {
                break;
            }
            kept += 1;
        }
    }
    return (text.take_front(end) + "…").str();
}

/// The first template argument list at the top level of a printed type,
/// with the text around it.
struct ArgumentList {
    llvm::StringRef head;
    llvm::SmallVector<llvm::StringRef, 4> arguments;
    llvm::StringRef tail;
};

std::optional<ArgumentList> first_argument_list(llvm::StringRef text) {
    std::optional<ArgumentList> list;
    llvm::SmallVector<char, 8> closers;
    std::size_t argument = 0;
    for(std::size_t i = 0; i < text.size(); i += 1) {
        switch(text[i]) {
            case '<':
                // Within parentheses or brackets, `<` and `>` compare.
                if(!closers.empty() && closers.back() != '>') {
                    break;
                }
                if(closers.empty() && !list) {
                    list.emplace().head = text.take_front(i);
                    argument = i + 1;
                }
                closers.push_back('>');
                break;
            case '(': closers.push_back(')'); break;
            case '[': closers.push_back(']'); break;
            case '>':
                // The arrow of a trailing return type closes nothing.
                if((i > 0 && text[i - 1] == '-') || (!closers.empty() && closers.back() != '>')) {
                    break;
                }
                [[fallthrough]];
            case ')':
            case ']':
                if(closers.empty() || closers.back() != text[i]) {
                    return std::nullopt;
                }
                closers.pop_back();
                if(list && closers.empty()) {
                    list->arguments.push_back(text.slice(argument, i).trim());
                    list->tail = text.drop_front(i + 1);
                    return list;
                }
                break;
            case ',':
                if(list && closers.size() == 1) {
                    list->arguments.push_back(text.slice(argument, i).trim());
                    argument = i + 1;
                }
                break;
            default: break;
        }
    }
    return std::nullopt;
}

/// `text`, a printed type, within `limit` characters: each template
/// argument keeps what of it fits, the rest of it written `…`, while the
/// arguments after it still have room for a `…` each; a run of `…` is
/// written once. Nullopt when not even `name<…>` fits.
std::optional<std::string> elide(llvm::StringRef text, std::size_t limit) {
    if(characters(text) <= limit) {
        return text.str();
    }
    auto list = first_argument_list(text);
    if(!list) {
        return std::nullopt;
    }
    auto fixed = characters(list->head) + characters(list->tail) + 2;
    if(fixed + 1 > limit) {
        return std::nullopt;
    }
    auto room = limit - fixed;
    std::string result = (list->head + "<").str();
    std::size_t used = 0;
    bool elided = false;
    for(auto [index, argument]: llvm::enumerate(list->arguments)) {
        std::size_t separator = index == 0 ? 0 : 2;
        std::size_t reserved = index + 1 < list->arguments.size() ? 3 : 0;
        std::optional<std::string> kept;
        if(used + separator + reserved < room) {
            kept = elide(argument, room - used - separator - reserved);
        }
        if(!kept) {
            if(elided) {
                continue;
            }
            kept = "…";
        }
        elided = *kept == "…";
        if(index != 0) {
            result += ", ";
        }
        result += *kept;
        used += separator + characters(*kept);
    }
    result += ">";
    result += list->tail;
    return result;
}

/// The core of a type an initializer names: what it points or refers to,
/// unqualified.
clang::QualType named_core(clang::QualType type) {
    type = type.getNonReferenceType();
    while(type->isPointerType()) {
        type = type->getPointeeType();
    }
    return type.getCanonicalType().getUnqualifiedType();
}

/// Whether a call names the type it returns among its explicit template
/// arguments: the type itself, behind pointers and references
/// (`any_cast<int>`, `create<Widget>`), or a specialization whose printed
/// arguments are all among them (`make_unique<Widget>` for a
/// `unique_ptr<Widget>`).
bool names_result(const clang::CallExpr* call, clang::QualType type) {
    llvm::SmallVector<clang::QualType, 2> written;
    for(const auto& argument:
        decls::explicit_template_arguments(call->getCallee()->IgnoreParenImpCasts())) {
        if(argument.getArgument().getKind() == clang::TemplateArgument::Type) {
            written.push_back(named_core(argument.getArgument().getAsType()));
        }
    }
    if(written.empty()) {
        return false;
    }
    auto core = named_core(type);
    if(llvm::is_contained(written, core)) {
        return true;
    }
    llvm::ArrayRef<clang::TemplateArgument> arguments;
    if(const auto* spec = llvm::dyn_cast_if_present<clang::ClassTemplateSpecializationDecl>(
           core->getAsRecordDecl())) {
        arguments = spec->getTemplateArgs().asArray();
    } else if(const auto* TST = core->getAs<clang::TemplateSpecializationType>()) {
        arguments = TST->template_arguments();
    }
    while(!arguments.empty() && arguments.back().getIsDefaulted()) {
        arguments = arguments.drop_back();
    }
    return !arguments.empty() &&
           llvm::all_of(arguments, [&](const clang::TemplateArgument& argument) {
               return argument.getKind() == clang::TemplateArgument::Type &&
                      llvm::is_contained(written, named_core(argument.getAsType()));
           });
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
            // Its written qualifier does not print: scopes are suppressed.
            if(auto* decl = TST->getTemplateName().getAsTemplateDecl()) {
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

    /// A qualifier's types print before the name; namespaces get no link.
    void qualifier(clang::NestedNameSpecifier qualifier) {
        if(qualifier.getKind() == clang::NestedNameSpecifier::Kind::Type) {
            walk(clang::QualType(qualifier.getAsType(), 0));
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
              CompilationUnit& unit,
              LocalSourceRange restrict_range,
              const InlayHintsOptions& options) :
        result(result), unit(unit), restrict_range(restrict_range), options(options) {}

    void run() {
        auto nodes = unit.get<Semantics>().node_entries();
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

        if(options.endif) {
            add_endif_hints();
        }
    }

private:
    // Compute the LSP range to attach the block end hint to, if any allowed.
    // 1. "}" is the last non-whitespace character on the line. The range of "}"
    // is returned.
    // 2. After "}", if the trimmed trailing text is exactly
    // `OptionalPunctuation`, say ";". The range of "} ... ;" is returned.
    // Otherwise, the hint shouldn't be shown.
    std::optional<LocalSourceRange> compute_block_end_range(clang::SourceRange brace_range,
                                                            llvm::StringRef optional_punctuation) {
        auto focus = unit.focus_file();
        auto block_begin = focus.offset(brace_range.getBegin());
        auto rbrace = focus.offset(brace_range.getEnd());

        // Because we need to check the block satisfies the minimum line limit, we
        // require both source location to be in the focus file. This prevents hint
        // to be shown in weird cases like '{' is actually in a "#include", but it's
        // rare anyway.
        if(!block_begin || !rbrace) {
            return std::nullopt;
        }
        auto rbrace_offset = *rbrace;

        llvm::StringRef rest_of_line = focus.content().substr(rbrace_offset).split('\n').first;
        if(!rest_of_line.starts_with("}")) {
            return std::nullopt;
        }

        llvm::StringRef trailing_text = rest_of_line.drop_front().trim();
        if(!trailing_text.empty() && trailing_text != optional_punctuation) {
            return std::nullopt;
        }

        auto& src_mgr = unit.context().getSourceManager();
        auto block_begin_line = src_mgr.getLineNumber(focus.file(), *block_begin);
        auto rbrace_line = src_mgr.getLineNumber(focus.file(), rbrace_offset);

        // Don't show hint on trivial blocks like `class X {};`
        if(block_begin_line + options.min_block_lines - 1 > rbrace_line) {
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
        auto focus = unit.focus_file();
        auto offset = focus.offset(expr->getBeginLoc());
        if(!offset) {
            return false;
        }

        llvm::StringRef content = focus.content().substr(0, *offset);

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

    bool should_hint_name(const clang::Expr* expr, llvm::StringRef name, bool unary) {
        if(name.empty())
            return false;

        if(!options.single_letter_names && unary && name.size() == 1) {
            return false;
        }

        // If the argument expression is a single name and it matches the
        // parameter name exactly, omit the name hint.
        auto spelled = display::identifier_of(expr);
        if(name == spelled)
            return false;

        // `aParam` is `param` behind a one-letter prefix.
        if(!options.prefixed_names && name.size() > 2 && clang::isLowercase(name[0]) &&
           clang::isUppercase(name[1]) && name.drop_front().equals_insensitive(spelled)) {
            return false;
        }

        // Exclude argument expressions preceded by a /*paramName*/.
        if(has_param_name_comment(expr, name)) {
            return false;
        }

        return true;
    }

    /// Whether an initializer already names the type it gives a variable:
    /// `long` in `static_cast<long>(x)`, `Widget` in `Widget{1, 2}`,
    /// `x<T>{}` and `new Widget`, a call naming it among its template
    /// arguments, a lambda its closure type. A class template's deduced
    /// arguments count as named where their own hint shows them.
    bool spells_type(const clang::Expr* init, clang::QualType type) const {
        init = init->IgnoreImplicit()->IgnoreParens();
        const clang::TypeSourceInfo* written = nullptr;
        if(const auto* cast = dyn_cast<clang::ExplicitCastExpr>(init)) {
            written = cast->getTypeInfoAsWritten();
        } else if(const auto* temporary = dyn_cast<clang::CXXTemporaryObjectExpr>(init)) {
            written = temporary->getTypeSourceInfo();
        } else if(const auto* unresolved = dyn_cast<clang::CXXUnresolvedConstructExpr>(init)) {
            written = unresolved->getTypeSourceInfo();
        } else if(const auto* allocation = dyn_cast<clang::CXXNewExpr>(init)) {
            written = allocation->getAllocatedTypeSourceInfo();
        } else if(const auto* call = dyn_cast<clang::CallExpr>(init)) {
            return names_result(call, type);
        } else {
            return llvm::isa<clang::LambdaExpr>(init);
        }
        // What a template's only instantiation makes of the written type is
        // news to the code.
        if(written->getType()->isDependentType() && !type->isDependentType()) {
            return false;
        }
        const auto* deduced = written->getType()->getContainedDeducedType();
        const auto* template_deduced =
            llvm::dyn_cast_if_present<clang::DeducedTemplateSpecializationType>(deduced);
        return !deduced || (template_deduced && shows_deduced_arguments(template_deduced));
    }

    /// Whether a type left to class template argument deduction gets its
    /// arguments hinted. An alias template's arguments are not its
    /// target's: `Twin` of `Pair<T, T>` takes one.
    bool shows_deduced_arguments(const clang::DeducedTemplateSpecializationType* type) const {
        return options.deduced_template_arguments &&
               llvm::isa_and_present<clang::ClassTemplateDecl>(
                   type->getTemplateName().getAsTemplateDecl());
    }

    bool hints_calls() const {
        return options.parameters || options.mutable_references || options.default_arguments;
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
            } else if(auto names = decls::named_parameters(*param); !names.empty()) {
                param = names.front();
                param_names.emplace_back(param->getName());
            } else {
                // Unnamed: an empty entry, the hint is dropped later.
                param_names.emplace_back();
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

        if(!hints_calls() || args.size() == 0) {
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
            const bool name_hint =
                options.parameters && should_hint_name(args[i], name, params.size() == 1);
            const bool reference_hint =
                options.mutable_references &&
                decls::binds_mutable_reference(params[i], forwarded_params[i]);

            const bool is_default = llvm::isa<clang::CXXDefaultArgExpr>(args[i]);
            has_non_default_args |= !is_default;
            if(is_default) {
                if(options.default_arguments) {
                    auto text = *display::default_argument(params[i]);
                    const auto limit = options.max_length.default_arguments;
                    auto& element = formatted_default_args.emplace_back();
                    if(name_hint) {
                        append(element, linked(name, named_params[i]));
                        append(element, plain(": "));
                    }
                    append(element, plain(limit && characters(text) > limit ? "…" : text));
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
            const auto limit = options.max_length.default_arguments;
            std::vector<InlayHintPart> hint;
            std::size_t size = 0;
            for(auto&& [index, element]: llvm::enumerate(formatted_default_args)) {
                std::size_t separator = hint.empty() ? 0 : 2;
                // While more defaults follow, `, …` must still fit after this one.
                std::size_t rest = index + 1 < formatted_default_args.size() ? 3 : 0;
                auto element_size = text_size(element);
                if(limit && size + separator + element_size + rest > limit) {
                    append(hint, plain(hint.empty() ? "…" : ", …"));
                    break;
                }
                if(separator) {
                    append(hint, plain(", "));
                }
                for(auto& part: element) {
                    append(hint, std::move(part));
                }
                size += separator + element_size;
            }

            add_inlay_hint(clang::SourceRange(rpunc_location),
                           HintSide::Left,
                           HintCategory::DefaultArgument,
                           has_non_default_args ? ", " : "",
                           std::move(hint),
                           "");
        }
    }

    /// `// name` after the closing brace of a block, linking to `start`.
    void add_block_end_hint(clang::SourceRange brace_range,
                            clang::SourceLocation start,
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

        add_end_hint(hint_range->end,
                     label,
                     options.max_length.block_end,
                     unit.focus_file().offset(start));
    }

    /// `// condition` after an `#endif`, the condition of the `#if` it
    /// closes, matched lexically: a preamble compile reports no directive
    /// it consumed, nor those in a skipped branch.
    void add_endif_hints() {
        const auto& lexical = unit.focus_file().lexical();
        for(const auto& directive: lexical.directives) {
            if(directive.kind == LexicalInfo::Directive::Kind::EndIf &&
               directive.opening != LexicalInfo::none) {
                add_endif_hint(lexical, lexical.directives[directive.opening], directive);
            }
        }
    }

    void add_endif_hint(const LexicalInfo& lexical,
                        const LexicalInfo::Directive& opening,
                        const LexicalInfo::Directive& closing) {
        auto content = unit.focus_file().content();
        if(content.slice(opening.range.begin, closing.range.begin).count('\n') + 1 <
           options.min_block_lines) {
            return;
        }

        // Nothing may follow the `endif`, a comment included.
        if(closing.last != closing.first + 2) {
            return;
        }

        auto words = lexical.words(opening);
        std::string condition = words.front().text(content) == "ifndef" ? "!" : "";
        std::optional<std::uint32_t> previous;
        for(const auto& token: llvm::drop_begin(words)) {
            if(previous && *previous < token.range.begin) {
                condition += ' ';
            }
            condition += token.text(content);
            previous = token.range.end;
        }
        if(previous) {
            add_end_hint(lexical.tokens[closing.first + 1].range.end,
                         condition,
                         options.max_length.endif,
                         opening.range.begin);
        }
    }

    void mark_block_end(const clang::Stmt* statement,
                        const clang::Stmt* body,
                        llvm::StringRef label,
                        llvm::StringRef name = "") {
        if(const auto* cs = llvm::dyn_cast_or_null<clang::CompoundStmt>(body)) {
            add_block_end_hint(cs->getSourceRange(), statement->getBeginLoc(), label, name, "");
        }
    }

    /// `// label` at `offset`, cut to `limit` characters, linking to `target`.
    void add_end_hint(std::uint32_t offset,
                      llvm::StringRef label,
                      std::uint32_t limit,
                      std::optional<std::uint32_t> target) {
        add_inlay_hint(LocalSourceRange(offset, offset),
                       HintSide::Right,
                       HintCategory::BlockEnd,
                       " // ",
                       {
                           InlayHintPart{.value = truncate(label, limit), .target = target}
        },
                       "");
    }

    static InlayHintPart plain(llvm::StringRef text) {
        return {.value = text.str()};
    }

    /// A label piece reaching `decl`, as the index knows it.
    InlayHintPart linked(llvm::StringRef text, const clang::NamedDecl* decl) {
        // Only a written declaration has index entries to reach.
        if(decl->getLocation().isInvalid()) {
            return plain(text);
        }
        if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(decl)) {
            decl = written_param(param);
        }
        const auto* symbol = decls::normalize(decl);
        InlayHintPart part{.value = text.str(), .symbol = unit.get<EntityTable>().entity(symbol)};
        if(index::classify_scope(symbol) != index::SymbolScope::External) {
            part.anchor = unit.file_path(unit.seen_at(symbol->getLocation()).file);
        }
        return part;
    }

    /// The parameter of the written function an instantiated one stands
    /// for: an instantiation's declarations have no index entries of their own.
    const static clang::ParmVarDecl* written_param(const clang::ParmVarDecl* param) {
        const auto* function = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
        const auto* pattern = function ? function->getTemplateInstantiationPattern() : nullptr;
        if(!pattern) {
            return param;
        }
        // A pack expands to any number of parameters, so past it indices
        // part ways; an instantiated parameter keeps the location of the
        // one it was written as, the parameters of a pack all the pack's.
        auto written = llvm::find_if(pattern->parameters(), [&](const auto* each) {
            return each->getLocation() == param->getLocation();
        });
        return written == pattern->parameters().end() ? param : *written;
    }

    /// Append a piece, merging plain text into the plain text before it.
    static void append(std::vector<InlayHintPart>& label, InlayHintPart part) {
        if(part.value.empty()) {
            return;
        }
        auto is_plain = [](const InlayHintPart& part) {
            return !part.symbol && !part.target;
        };
        if(!label.empty() && is_plain(label.back()) && is_plain(part)) {
            label.back().value += part.value;
            return;
        }
        label.push_back(std::move(part));
    }

    static std::size_t text_size(llvm::ArrayRef<InlayHintPart> label) {
        std::size_t size = 0;
        for(const auto& part: label) {
            size += characters(part.value);
        }
        return size;
    }

    /// The pieces of `text`, the printed form of `type`: a name the printer
    /// wrote for a declaration links to it, the rest stays plain.
    std::vector<InlayHintPart> type_label(clang::QualType type, llvm::StringRef text) {
        SpelledNames spelled;
        spelled.qualifier(display::restored_scope(type, display_options));
        spelled.walk(type);
        // Once elision has cut a name out, a namesake of another declaration
        // can take its place in the search: such names stay plain.
        auto ambiguous = [&](llvm::StringRef name, const clang::NamedDecl* decl) {
            return text.contains("…") && llvm::any_of(spelled.names, [&](const SpelledName& other) {
                       return other.name == name &&
                              other.decl->getCanonicalDecl() != decl->getCanonicalDecl();
                   });
        };
        std::vector<InlayHintPart> label;
        std::size_t cursor = 0;
        for(const auto& [name, decl]: spelled.names) {
            if(ambiguous(name, decl)) {
                continue;
            }
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
        auto local_range = unit.focus_file().written_range(range);
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

    std::optional<std::string> fit_type(llvm::StringRef text) const {
        auto limit = options.max_length.types;
        return limit == 0 ? text.str() : elide(text, limit);
    }

    void add_type_hint(clang::SourceRange range, clang::QualType type, llvm::StringRef prefix) {
        if(!options.deduced_types || type.isNull()) {
            return;
        }
        // The desugared type, or the sugared one when that fits instead or
        // when only it can be shortened.
        auto sugared = readable_types.readable(type);
        auto printed = readable_types.readable(display::maybe_desugar(unit.context(), type));
        auto text = display::type(unit.context(), printed, display_options).text;
        if(auto limit = options.max_length.types;
           limit != 0 && characters(text) > limit && sugared != printed) {
            auto sugared_text = display::type(unit.context(), sugared, display_options).text;
            if(characters(sugared_text) <= limit || !elide(text, limit)) {
                printed = sugared;
                text = std::move(sugared_text);
            }
        }
        if(auto fitted = fit_type(text)) {
            add_inlay_hint(range,
                           HintSide::Right,
                           HintCategory::Type,
                           prefix,
                           type_label(printed, *fitted),
                           /*Suffix=*/"");
        }
    }

    /// Hint the unwritten designators of the initializers of an aggregate,
    /// e.g. `.x` for the `1` in `Point{1}`. Explicitly written designators
    /// and inits already carrying a `/*name=*/` comment stay bare.
    void add_designators(clang::QualType aggregate,
                         llvm::ArrayRef<const clang::Expr*> inits,
                         const llvm::DenseMap<clang::SourceLocation, std::string>& designators) {
        for(const clang::Expr* init: inits) {
            if(llvm::isa<clang::DesignatedInitExpr>(init)) {
                continue;
            }
            auto it = designators.find(init->getBeginLoc());
            if(it == designators.end() || has_param_name_comment(init, it->second)) {
                continue;
            }
            auto label = designator_label(aggregate, it->second);
            if(label.empty()) {
                continue;
            }
            add_inlay_hint(init->getSourceRange(),
                           HintSide::Left,
                           HintCategory::Designator,
                           /*Prefix=*/"",
                           std::move(label),
                           /*Suffix=*/"=");
        }
    }

    /// The pieces of a designator such as `.inner.x` or `[2].x`: each field
    /// name links to the field, looked up from the aggregate's type down.
    /// Without field designators a path through a field shows nothing: its
    /// indices alone would repeat over the fields.
    std::vector<InlayHintPart> designator_label(clang::QualType aggregate,
                                                llvm::StringRef designator) {
        std::vector<InlayHintPart> label;
        if(!options.designators && designator.contains('.')) {
            return label;
        }
        while(!designator.empty()) {
            if(designator.consume_front("[")) {
                auto index = designator.take_until([](char c) { return c == ']'; });
                designator = designator.drop_front(index.size() + 1);
                if(options.array_indices) {
                    append(label, plain(("[" + index + "]").str()));
                }
                // The lone member array of `std::array` stays undesignated:
                // `[i]` then indexes the record itself.
                const auto* array =
                    aggregate.isNull() ? nullptr : aggregate->getAsArrayTypeUnsafe();
                aggregate = array ? array->getElementType() : clang::QualType();
                continue;
            }
            designator.consume_front(".");
            auto name = designator.take_until([](char c) { return c == '.' || c == '['; });
            designator = designator.drop_front(name.size());
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
            append(label, plain("."));
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
        } else if(auto* expr = dyn_cast<clang::ExplicitCastExpr>(stmt)) {
            add_deduced_arguments_hint(expr->getTypeInfoAsWritten(), expr->getType());
        } else if(auto* expr = dyn_cast<clang::CXXNewExpr>(stmt)) {
            add_deduced_arguments_hint(expr->getAllocatedTypeSourceInfo(),
                                       expr->getAllocatedType());
        } else if(auto* expr = dyn_cast<clang::CXXStdInitializerListExpr>(stmt)) {
            // The elements of a std::initializer_list have no designators
            // to write.
            initializer_lists.insert(
                llvm::dyn_cast<clang::InitListExpr>(expr->getSubExpr()->IgnoreImplicit()));
        } else if(auto* expr = dyn_cast<clang::InitListExpr>(stmt)) {
            handle_init_list(expr);
        } else if(auto* expr = dyn_cast<clang::CXXParenListInitExpr>(stmt)) {
            handle_paren_list_init(expr);
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
        // `namespace a::b {` declares `b` as the only member of `a`; both end
        // at one brace, which the outermost names.
        if(!options.namespace_end || decl->isNested()) {
            return;
        }
        auto nested = [](const clang::NamespaceDecl* outer) -> const clang::NamespaceDecl* {
            auto first = outer->decls_begin();
            const auto* inner = first == outer->decls_end()
                                    ? nullptr
                                    : llvm::dyn_cast<clang::NamespaceDecl>(*first);
            return inner && inner->isNested() ? inner : nullptr;
        };
        std::string name = display::identifier_of(decl).str();
        for(const auto* inner = nested(decl); inner; inner = nested(inner)) {
            name += "::";
            name += display::identifier_of(inner);
        }
        // For namespace, the range actually starts at the namespace keyword. But
        // it should be fine since it's usually very short.
        // Anonymous namespaces hint as plain "// namespace".
        add_block_end_hint(decl->getSourceRange(), decl->getLocation(), "namespace", name, "");
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
            add_block_end_hint(decl->getBraceRange(),
                               decl->getLocation(),
                               prefix,
                               display::identifier_of(decl),
                               ";");
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
                add_block_end_hint(body->getSourceRange(),
                                   decl->getLocation(),
                                   "",
                                   display::name_of(decl),
                                   "");
            }
        }
    }

    void handle_var(const clang::VarDecl* var) {
        // Do not show hints for the aggregate in a structured binding,
        // but show hints for the individual bindings.
        if(auto* decl = dyn_cast<clang::DecompositionDecl>(var)) {
            // The bindings come out of the type the code wrote for what it
            // decomposes: a range-for's range, else the initializer.
            WrittenTypes written_types(unit.context());
            auto range = range_inits.find(decl);
            const auto* written = range != range_inits.end() ? range->second : decl->getInit();
            if(written) {
                written_types.collect(written->getType());
            }
            for(auto* binding: decl->bindings()) {
                auto type = binding->getType();
                if(type.isNull() || type->isDependentType()) {
                    continue;
                }
                // A tuple-like binding is declared `std::tuple_element<I, E>::type`,
                // whatever the library's sugar beneath names nothing the code wrote.
                if(binding->getHoldingVar()) {
                    type = type.getCanonicalType();
                }
                add_type_hint(binding->getLocation(),
                              written_types.as_written(type),
                              /*Prefix=*/": ");
            }
            return;
        }

        auto type = var->getType();
        add_deduced_arguments_hint(var->getTypeSourceInfo(), type);
        if(type->getContainedAutoType()) {
            // A dependent initializer leaves `auto` to instantiation; the
            // resolver tells the type where the template's only instantiation
            // or a class template's members do, else as the template writes it.
            auto deduced = !type->isDependentType() ? type
                           : var->getInit() ? unit.get<types::TemplateResolver>().deduce(var)
                                            : clang::QualType();
            if(!deduced.isNull() && types::writable(deduced) &&
               (options.spelled_types || !var->getInit() ||
                !spells_type(var->getInit(), deduced))) {
                // Our current approach is to place the hint on the variable
                // and accordingly print the full type
                // (e.g. for `const auto& x = 42`, print `const int&`).
                // Alternatively, we could place the hint on the `auto`
                // (and then just print the type deduced for the `auto`).
                add_type_hint(var->getLocation(), deduced, /*Prefix=*/": ");
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
        if(const auto* temporary = llvm::dyn_cast<clang::CXXTemporaryObjectExpr>(expr)) {
            add_deduced_arguments_hint(temporary->getTypeSourceInfo(), expr->getType());
        }

        // Weed out constructor calls that don't look like a function call with
        // an argument list, by checking the validity of getParenOrBraceRange().
        // Also weed out std::initializer_list constructors as there are no names
        // for the individual arguments.
        if(!expr->getParenOrBraceRange().isValid() || expr->isStdInitListInitialization()) {
            return;
        }

        add_params({.decl = decls::constructor_of(expr)},
                   expr->getParenOrBraceRange().getEnd(),
                   {expr->getArgs(), expr->getNumArgs()});
    }

    void handle_call(const clang::CallExpr* expr) {
        if(!hints_calls()) {
            return;
        }

        auto isFunctionObjectCallExpr = [](const clang::CallExpr* E) {
            if(auto* call_expr = dyn_cast<clang::CXXOperatorCallExpr>(E)) {
                return call_expr->getOperator() == clang::OverloadedOperatorKind::OO_Call;
            }

            return false;
        };

        bool is_functor = isFunctionObjectCallExpr(expr);

        // A call on an object the code cannot name is not written there:
        // `co_yield x` calls the coroutine promise's `yield_value(x)`.
        if(const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
            const auto* object = member->getImplicitObjectArgument();
            const auto* ref =
                object ? llvm::dyn_cast<clang::DeclRefExpr>(object->IgnoreImplicit()) : nullptr;
            if(ref && ref->getDecl()->isImplicit()) {
                return;
            }
        }

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
            auto candidates = unit.get<types::TemplateResolver>().lookup(expr);
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
        } else if(auto loc = clang::HeuristicResolver(unit.context())
                                 .getFunctionProtoTypeLoc(expr->getCallee())) {
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
            mark_block_end(S, S->getBody(), "for", name);
        }
    }

    void handle_range_for(const clang::CXXForRangeStmt* S) {
        range_inits.try_emplace(S->getLoopVariable(), S->getRangeInit());
        if(options.block_end) {
            // Decomposition loop variables have no identifier: plain "// for".
            mark_block_end(S, S->getBody(), "for", display::identifier_of(S->getLoopVariable()));
        }
    }

    void handle_while(const clang::WhileStmt* S) {
        if(options.block_end) {
            mark_block_end(S, S->getBody(), "while", display::summarize(S->getCond()));
        }
    }

    void handle_switch(const clang::SwitchStmt* S) {
        if(options.block_end) {
            mark_block_end(S, S->getBody(), "switch", display::summarize(S->getCond()));
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
                                   S->getBeginLoc(),
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
        if(initializer_lists.contains(expr)) {
            return;
        }

        // The table records the form the enclosing AST node stores, which for
        // the outermost list is the *semantic* form. Designators attach to the
        // syntactic form — the written one, which may have subobject
        // initializers inlined without braces where the semantic form has
        // nested init-lists. designators::unwritten looks at the semantic
        // form to determine the labels.
        if(const auto* syntactic = expr->getSyntacticForm()) {
            expr = syntactic;
        }

        if((!options.designators && !options.array_indices) ||
           expr->isIdiomaticZeroInitializer(unit.lang_options())) {
            return;
        }

        add_designators(expr->getType(), expr->inits(), designators::unwritten(expr));
    }

    void handle_paren_list_init(const clang::CXXParenListInitExpr* expr) {
        if(options.designators || options.array_indices) {
            add_designators(expr->getType(),
                            expr->getUserSpecifiedInitExprs(),
                            designators::unwritten(expr));
        }
    }

    void handle_type_loc(clang::TypeLoc TL) {
        if(const auto* DT = llvm::dyn_cast<clang::DecltypeType>(TL.getType()))
            if(clang::QualType UT = DT->getUnderlyingType(); !UT->isDependentType())
                add_type_hint(TL.getSourceRange(), UT, ": ");
    }

    /// The template arguments class template argument deduction found for
    /// a `written` type, `<int, double>` after the `Pair` of
    /// `Pair pair(1, 2.5)`; the written type keeps the bare template name,
    /// `deduced` is what its owner got.
    void add_deduced_arguments_hint(const clang::TypeSourceInfo* written, clang::QualType deduced) {
        if(!written || deduced->isDependentType()) {
            return;
        }
        auto loc = written->getTypeLoc()
                       .getUnqualifiedLoc()
                       .getAs<clang::DeducedTemplateSpecializationTypeLoc>();
        if(!loc || !shows_deduced_arguments(loc.getTypePtr())) {
            return;
        }
        const auto* spec = llvm::dyn_cast_if_present<clang::ClassTemplateSpecializationDecl>(
            deduced->getAsCXXRecordDecl());
        if(!spec) {
            return;
        }
        // The arguments as the template's own name takes them: without the
        // scope around it (`Outer<int>::`), the standard alias naming the
        // whole (`string`), or the sugar of a deduction guide's return type
        // (`enable_if_t<…>` for `std::array`).
        auto& context = unit.context();
        auto arguments = spec->getTemplateArgs().asArray();
        // A guide may pass the defaults explicitly (`basic_string`'s traits).
        const auto* params = spec->getSpecializedTemplate()->getTemplateParameters();
        while(!arguments.empty() && arguments.size() <= params->size() &&
              clang::isSubstitutedDefaultArgument(context,
                                                  arguments.back(),
                                                  params->getParam(arguments.size() - 1),
                                                  arguments.drop_back(),
                                                  params->getDepth())) {
            arguments = arguments.drop_back();
        }
        auto named = readable_types.readable(context.getTemplateSpecializationType(
            clang::ElaboratedTypeKeyword::None,
            clang::TemplateName(spec->getSpecializedTemplate()),
            arguments,
            spec->getTemplateArgs().asArray(),
            context.getCanonicalType(deduced)));
        auto text = display::type(context, named, display_options).text;
        auto list = first_argument_list(text);
        if(!list) {
            return;
        }
        auto fitted = fit_type(llvm::StringRef(text).drop_front(list->head.size()));
        if(!fitted) {
            return;
        }
        // Linked with the name in front, which then goes: the name of an
        // argument can be the template's own.
        auto label = type_label(named, (list->head + *fitted).str());
        for(auto head = list->head.size(); head != 0;) {
            auto& first = label.front();
            if(first.value.size() > head) {
                first.value.erase(0, head);
                break;
            }
            head -= first.value.size();
            label.erase(label.begin());
        }
        add_inlay_hint(loc.getSourceRange(),
                       HintSide::Right,
                       HintCategory::Type,
                       "",
                       std::move(label),
                       "");
    }

    // FIXME: Handle RecoveryExpr to try to hint some invalid calls.

private:
    std::vector<InlayHint>& result;
    CompilationUnit& unit;
    LocalSourceRange restrict_range;
    const InlayHintsOptions& options;

    // The sugared type is more useful in some cases, and the canonical
    // type in other cases. Suppress scopes to keep type names short.
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
    /// A range-for's loop variable, visited after the loop, by its range.
    llvm::DenseMap<const clang::VarDecl*, const clang::Expr*> range_inits;
    /// The arrays behind std::initializer_list objects, visited after them.
    llvm::DenseSet<const clang::InitListExpr*> initializer_lists;

    display::ReadableTypes readable_types{unit.context(), display_options};
};

}  // namespace

auto inlay_hints(CompilationUnit& unit, LocalSourceRange target, const InlayHintsOptions& options)
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

    // Editors show a few dozen characters of hints on a line: the thousands
    // of a generated one-line table would only be sent to be dropped.
    constexpr std::size_t line_limit = 100;
    auto content = unit.focus_file().content();
    std::uint32_t scanned = 0;
    std::size_t on_line = 0;
    std::vector<InlayHint> hints;
    hints.reserve(raw_hints.size());
    for(auto& hint: raw_hints) {
        if(content.slice(scanned, hint.offset).contains('\n')) {
            on_line = 0;
        }
        scanned = hint.offset;
        on_line += 1;
        if(on_line <= line_limit) {
            hints.push_back(std::move(hint));
        }
    }
    return hints;
}

auto inlay_hints_to_protocol(
    llvm::ArrayRef<InlayHint> hints,
    const PositionMap& map,
    PositionEncoding encoding,
    llvm::function_ref<std::optional<protocol::Location>(const InlayHintPart&)> locate)
    -> std::vector<protocol::InlayHint> {
    std::vector<protocol::InlayHint> result;
    result.reserve(hints.size());

    for(const auto& hint: hints) {
        auto pos = map.position(hint.offset, encoding);
        if(!pos)
            continue;
        protocol::InlayHint out{.position = *pos};

        auto links = [](const InlayHintPart& part) {
            return part.symbol != 0 || part.target;
        };
        if(locate && llvm::any_of(hint.label, links)) {
            std::vector<protocol::InlayHintLabelPart> parts;
            for(const auto& part: hint.label) {
                parts.push_back(
                    {.value = part.value, .location = links(part) ? locate(part) : std::nullopt});
            }
            out.label = std::move(parts);
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
