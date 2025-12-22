#include "Feature/InlayHint.h"

#include "AST/FilterASTVisitor.h"
#include "AST/Utility.h"
#include "Support/Compare.h"
#include "Support/Format.h"

#include "llvm/ADT/StringExtras.h"
#include "clang/Lex/Lexer.h"

namespace clice::feature {

namespace {

// For now, inlay hints are always anchored at the left or right of their range.
enum class HintSide { Left, Right };

bool is_expanded_from_param_pack(const clang::ParmVarDecl* param) {
    return ast::underlying_pack_type(param) != nullptr;
}

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

// If "E" spells a single unqualified identifier, return that name.
// Otherwise, return an empty string.
llvm::StringRef spelled_identifier_of(const clang::Expr* expr) {
    expr = expr->IgnoreUnlessSpelledInSource();

    if(auto* dre = dyn_cast<clang::DeclRefExpr>(expr))
        if(!dre->getQualifier())
            return ast::identifier_of(*dre->getDecl());

    if(auto* me = dyn_cast<clang::MemberExpr>(expr))
        if(!me->getQualifier() && me->isImplicitAccess())
            return ast::identifier_of(*me->getMemberDecl());

    return {};
}

bool is_setter(const clang::FunctionDecl* callee,
               const llvm::SmallVector<llvm::StringRef, 8>& names) {
    if(names.size() != 1) {
        return false;
    }

    llvm::StringRef name = ast::identifier_of(*callee);
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
bool is_simple_builtin(const clang::FunctionDecl* callee) {
    switch(callee->getBuiltinID()) {
        case clang::Builtin::BIaddressof:
        case clang::Builtin::BIas_const:
        case clang::Builtin::BIforward:
        case clang::Builtin::BImove:
        case clang::Builtin::BImove_if_noexcept: return true;
        default: return false;
    }
}

struct Callee {
    // Only one of Decl or Loc is set.
    // Loc is for calls through function pointers.
    const clang::FunctionDecl* decl = nullptr;
    clang::FunctionProtoTypeLoc loc;
};

class Builder {
public:
    Builder(std::vector<InlayHint>& result,
            CompilationUnit& unit,
            LocalSourceRange restrict_range,
            const config::InlayHintsOptions& options) :
        result(result), unit(unit), restrict_range(restrict_range), options(options),
        policy(unit.context().getPrintingPolicy()) {
        // The sugared type is more useful in some cases, and the canonical
        // type in other cases.
        policy.SuppressScope = true;           // keep type names short
        policy.AnonymousTagLocations = false;  // do not print lambda locations
        // Not setting PrintCanonicalTypes for "auto" allows
        // SuppressDefaultTemplateArgs (set by default) to have an effect.
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
        if(begin_fid != end_fid || begin_fid != unit.interested_file()) {
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
        if(block_begin_fid != rbrace_fid || block_begin_fid != unit.interested_file()) {
            return std::nullopt;
        }

        llvm::StringRef rest_of_line =
            unit.interested_content().substr(rbrace_offset).split('\n').first;
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
        if(fid != unit.interested_file()) {
            return false;
        }

        llvm::StringRef content = unit.interested_content().substr(0, offset);

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
        if(name == spelled_identifier_of(expr))
            return false;

        // Exclude argument expressions preceded by a /*paramName*/.
        if(has_param_name_comment(expr, name)) {
            return false;
        }

        return true;
    }

    bool should_hint_reference(const clang::ParmVarDecl* param,
                               const clang::ParmVarDecl* forwarded_param) {
        // We add a & hint only when the argument is passed as mutable reference.
        // For parameters that are not part of an expanded pack, this is
        // straightforward. For expanded pack parameters, it's likely that they will
        // be forwarded to another function. In this situation, we only want to add
        // the reference hint if the argument is actually being used via mutable
        // reference. This means we need to check
        // 1. whether the value category of the argument is preserved, i.e. each
        //    pack expansion uses std::forward correctly.
        // 2. whether the argument is ever copied/cast instead of passed
        //    by-reference
        // Instead of checking this explicitly, we use the following proxy:
        // 1. the value category can only change from rvalue to lvalue during
        //    forwarding, so checking whether both the parameter of the forwarding
        //    function and the forwarded function are lvalue references detects such
        //    a conversion.
        // 2. if the argument is copied/cast somewhere in the chain of forwarding
        //    calls, it can only be passed on to an rvalue reference or const lvalue
        //    reference parameter. Thus if the forwarded parameter is a mutable
        //    lvalue reference, it cannot have been copied/cast to on the way.
        // Additionally, we should not add a reference hint if the forwarded
        // parameter was only partially resolved, i.e. points to an expanded pack
        // parameter, since we do not know how it will be used eventually.
        auto type = param->getType();
        auto forwarded_type = forwarded_param->getType();
        return type->isLValueReferenceType() && forwarded_type->isLValueReferenceType() &&
               !forwarded_type.getNonReferenceType().isConstQualified() &&
               !is_expanded_from_param_pack(forwarded_param);
    }

    using NameVec = llvm::SmallVector<llvm::StringRef, 8>;

    NameVec choose_param_names(llvm::ArrayRef<const clang::ParmVarDecl*> params) {
        NameVec param_names;
        for(const auto* param: params) {
            if(is_expanded_from_param_pack(param)) {
                // If we haven't resolved a pack paramater (e.g. foo(Args... args)) to a
                // non-pack parameter, then hinting as foo(args: 1, args: 2, args: 3) is
                // unlikely to be useful.
                param_names.emplace_back();
            } else {
                auto simple_name = ast::identifier_of(*param);
                // If the parameter is unnamed in the declaration:
                // attempt to get its name from the definition
                if(simple_name.empty()) {
                    if(const auto* def = param_definition(param)) {
                        simple_name = ast::identifier_of(*def);
                    }
                }
                param_names.emplace_back(simple_name);
            }
        }

        // Standard library functions often have parameter names that start
        // with underscores, which makes the hints noisy, so strip them out.
        for(auto& name: param_names) {
            name = name.ltrim('_');
        }

        return param_names;
    }

public:
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

        llvm::SmallVector<std::string> formatted_default_args;
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
            forwarded_params_storage = ast::resolve_forwarding_params(callee.decl);
            forwarded_params = remove_self_params(forwarded_params_storage);
        } else {
            params = remove_self_params(callee.loc.getParams());
            forwarded_params = {params.begin(), params.end()};
        }

        NameVec param_names = choose_param_names(forwarded_params);

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
                should_hint_reference(params[i], forwarded_params[i]) && options.parameters;

            const bool is_default = llvm::isa<clang::CXXDefaultArgExpr>(args[i]);
            has_non_default_args |= !is_default;
            if(is_default) {
                if(options.default_arguments) {
                    const auto text = clang::Lexer::getSourceText(
                        clang::CharSourceRange::getTokenRange(params[i]->getDefaultArgRange()),
                        unit.context().getSourceManager(),
                        unit.lang_options());
                    const auto abbrev =
                        (text.size() > options.type_name_limit || text.contains("\n")) ? "..."
                                                                                       : text;
                    if(name_hint) {
                        formatted_default_args.emplace_back(std::format("{0}: {1}", name, abbrev));
                    } else {
                        formatted_default_args.emplace_back(std::format("{0}", abbrev));
                    }
                }
            } else if(name_hint || reference_hint) {
                add_inlay_hint(args[i]->getSourceRange(),
                               HintSide::Left,
                               InlayHintKind::Parameter,
                               reference_hint ? "&" : "",
                               name_hint ? name : "",
                               ": ");
            }
        }

        if(!formatted_default_args.empty()) {
            std::string hint;
            llvm::raw_string_ostream os(hint);
            llvm::ListSeparator sep(", ");
            for(auto&& element: formatted_default_args) {
                os << sep;
                if(hint.size() + element.size() >= options.type_name_limit) {
                    os << "...";
                    break;
                }
                os << element;
            }
            os.flush();

            add_inlay_hint(clang::SourceRange(rpunc_location),
                           HintSide::Left,
                           InlayHintKind::DefaultArgument,
                           has_non_default_args ? ", " : "",
                           hint,
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

        add_inlay_hint(*hint_range, HintSide::Right, InlayHintKind::BlockEnd, " // ", label, "");
    }

    void mark_block_end(const clang::Stmt* body, llvm::StringRef label, llvm::StringRef name = "") {
        if(const auto* cs = llvm::dyn_cast_or_null<clang::CompoundStmt>(body)) {
            add_block_end_hint(cs->getSourceRange(), label, name, "");
        }
    }

    // We pass HintSide rather than SourceLocation because we want to ensure
    // it is in the same file as the common file range.
    void add_inlay_hint(clang::SourceRange range,
                        HintSide side,
                        InlayHintKind Kind,
                        llvm::StringRef prefix,
                        llvm::StringRef label,
                        llvm::StringRef suffix) {
        auto local_range = hint_range(range);
        if(!local_range) {
            return;
        }

        add_inlay_hint(*local_range, side, Kind, prefix, label, suffix);
    }

    void add_inlay_hint(LocalSourceRange range,
                        HintSide side,
                        InlayHintKind kind,
                        llvm::StringRef prefix,
                        llvm::StringRef label,
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

        InlayHint hint{offset, kind};
        hint.parts.emplace_back(-1, (prefix + label + suffix).str());
        result.push_back(std::move(hint));
    }

    void add_type_hint(clang::SourceRange range, clang::QualType type, llvm::StringRef prefix) {
        if(!options.deduced_types || type.isNull())
            return;

        auto desugared = ast::maybe_desugar(unit.context(), type);
        std::string type_name = desugared.getAsString(policy);

        auto should_print = [&](llvm::StringRef TypeName) {
            return options.type_name_limit == 0 || TypeName.size() < options.type_name_limit;
        };

        if(type != desugared && !should_print(type_name)) {
            // If the desugared type is too long to display, fallback to the sugared
            // type.
            type_name = type.getAsString(policy);
        }

        if(should_print(type_name)) {
            add_inlay_hint(range,
                           HintSide::Right,
                           InlayHintKind::Type,
                           prefix,
                           type_name,
                           /*Suffix=*/"");
        }
    }

    void add_designator_hint(clang::SourceRange range, llvm::StringRef text) {
        add_inlay_hint(range,
                       HintSide::Left,
                       InlayHintKind::Designator,
                       /*Prefix=*/"",
                       text,
                       /*Suffix=*/"=");
    }

    void add_return_type_hint(clang::FunctionDecl* decl, clang::SourceRange range) {
        auto* type = decl->getReturnType()->getContainedAutoType();
        if(!type || type->getDeducedType().isNull()) {
            return;
        }
        add_type_hint(range, decl->getReturnType(), /*Prefix=*/"-> ");
    }

private:
    std::vector<InlayHint>& result;
    CompilationUnit& unit;
    LocalSourceRange restrict_range;
    const config::InlayHintsOptions& options;
    clang::PrintingPolicy policy;
};

class Visitor : public FilteredASTVisitor<Visitor> {
public:
    using Base = FilteredASTVisitor<Visitor>;

    Visitor(Builder& builder,
            CompilationUnit& unit,
            std::optional<LocalSourceRange> restrict_range,
            const config::InlayHintsOptions& options) :
        Base(unit, true), builder(builder), unit(unit), options(options) {}

public:
    // Carefully recurse into PseudoObjectExprs, which typically incorporate
    // a syntactic expression and several semantic expressions.
    bool TraversePseudoObjectExpr(clang::PseudoObjectExpr* expr) {
        clang::Expr* syntactic_expr = expr->getSyntacticForm();
        if(llvm::isa<clang::CallExpr>(syntactic_expr)) {
            // Since the counterpart semantics usually get the identical source
            // locations as the syntactic one, visiting those would end up presenting
            // confusing hints e.g., __builtin_dump_struct.
            // Thus, only traverse the syntactic forms if this is written as a
            // CallExpr. This leaves the door open in case the arguments in the
            // syntactic form could possibly get parameter names.
            return Base::TraverseStmt(syntactic_expr);
        }

        // We don't want the hints for some of the MS property extensions.
        // e.g.
        // struct S {
        //   __declspec(property(get=GetX, put=PutX)) int x[];
        //   void PutX(int y);
        //   void Work(int y) { x = y; } // Bad: `x = y: y`.
        // };
        if(llvm::isa<clang::BinaryOperator>(syntactic_expr)) {
            return true;
        }

        // FIXME: Handle other forms of a pseudo object expression.
        return Base::TraversePseudoObjectExpr(expr);
    }

    bool VisitNamespaceDecl(clang::NamespaceDecl* decl) {
        if(options.block_end) {
            // For namespace, the range actually starts at the namespace keyword. But
            // it should be fine since it's usually very short.
            builder.add_block_end_hint(decl->getSourceRange(),
                                       "namespace",
                                       ast::identifier_of(*decl),
                                       "");
        }
        return true;
    }

    bool VisitTagDecl(clang::TagDecl* decl) {
        if(options.block_end && decl->isThisDeclarationADefinition()) {
            std::string prefix = decl->getKindName().str();

            if(const auto* enum_decl = dyn_cast<clang::EnumDecl>(decl)) {
                if(enum_decl->isScoped()) {
                    prefix += enum_decl->isScopedUsingClassTag() ? " class" : " struct";
                }
            };

            builder.add_block_end_hint(decl->getBraceRange(),
                                       prefix,
                                       ast::identifier_of(*decl),
                                       ";");
        }
        return true;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        if(auto* proto_type = llvm::dyn_cast<clang::FunctionProtoType>(decl->getType())) {
            if(!proto_type->hasTrailingReturn()) {
                if(auto FTL = decl->getFunctionTypeLoc()) {
                    builder.add_return_type_hint(decl, FTL.getRParenLoc());
                }
            }
        }

        if(options.block_end && decl->isThisDeclarationADefinition()) {
            // We use `printName` here to properly print name of ctor/dtor/operator
            // overload.
            if(const clang::Stmt* body = decl->getBody()) {
                builder.add_block_end_hint(body->getSourceRange(),
                                           "",
                                           ast::display_name_of(decl),
                                           "");
            }
        }

        return true;
    }

    bool VisitVarDecl(clang::VarDecl* var) {
        // Do not show hints for the aggregate in a structured binding,
        // but show hints for the individual bindings.
        if(auto* decl = dyn_cast<clang::DecompositionDecl>(var)) {
            for(auto* binding: decl->bindings()) {
                // For structured bindings, print canonical types. This is important
                // because for bindings that use the tuple_element protocol, the
                // non-canonical types would be "tuple_element<I, A>::type".
                if(auto type = binding->getType(); !type.isNull() && !type->isDependentType()) {
                    builder.add_type_hint(binding->getLocation(),
                                          type.getCanonicalType(),
                                          /*Prefix=*/": ");
                }
            }
            return true;
        }

        auto type = var->getType();
        if(auto* auto_type = type->getContainedAutoType()) {
            if(auto_type->isDeduced() && !type->isDependentType()) {
                // Our current approach is to place the hint on the variable
                // and accordingly print the full type
                // (e.g. for `const auto& x = 42`, print `const int&`).
                // Alternatively, we could place the hint on the `auto`
                // (and then just print the type deduced for the `auto`).
                builder.add_type_hint(var->getLocation(), var->getType(), /*Prefix=*/": ");
            }
        }

        // Handle templates like `int foo(auto x)` with exactly one instantiation.
        if(auto* param = llvm::dyn_cast<clang::ParmVarDecl>(var)) {
            if(var->getIdentifier() && type->isDependentType()) {
                auto unwrapped = ast::unwrap_type(var->getTypeSourceInfo()->getTypeLoc());
                if(auto type = unwrapped.getAs<clang::TemplateTypeParmTypeLoc>()) {
                    if(auto decl = type.getDecl(); decl && decl->isImplicit()) {
                        if(auto* IPVD = ast::get_only_instantiation(param)) {
                            builder.add_type_hint(var->getLocation(),
                                                  IPVD->getType(),
                                                  /*Prefix=*/": ");
                        }
                    }
                }
            }
        }

        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* expr) {
        // Weed out constructor calls that don't look like a function call with
        // an argument list, by checking the validity of getParenOrBraceRange().
        // Also weed out std::initializer_list constructors as there are no names
        // for the individual arguments.
        if(!expr->getParenOrBraceRange().isValid() || expr->isStdInitListInitialization()) {
            return true;
        }

        Callee callee;
        callee.decl = expr->getConstructor();
        if(!callee.decl) {
            return true;
        }

        builder.add_params(callee,
                           expr->getParenOrBraceRange().getEnd(),
                           {expr->getArgs(), expr->getNumArgs()});
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* expr) {
        if(!options.parameters) {
            return true;
        }

        auto isFunctionObjectCallExpr = [](clang::CallExpr* E) {
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
            return true;

        /// FIXME: Use template resolver here.
        if(expr->isTypeDependent() || expr->isValueDependent()) {
            return true;
        }

        auto callee_decl = expr->getCalleeDecl();

        Callee callee;
        if(const auto* FD = llvm::dyn_cast<clang::FunctionDecl>(callee_decl)) {
            callee.decl = FD;
        } else if(const auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(callee_decl)) {
            callee.decl = FTD->getTemplatedDecl();
        } else if(clang::FunctionProtoTypeLoc loc = ast::proto_type_loc(expr->getCallee())) {
            callee.loc = loc;
        } else {
            return true;
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

        // We don't have the implied object argument through a function pointer either.
        if(const auto* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee.decl)) {
            if(is_functor || method->hasCXXExplicitFunctionObjectParameter()) {
                args = args.drop_front(1);
            }
        }

        builder.add_params(callee, expr->getRParenLoc(), args);
        return true;
    }

    bool VisitForStmt(clang::ForStmt* S) {
        if(options.block_end) {
            std::string name;
            // Common case: for (int I = 0; I < N; I++). Use "I" as the name.
            if(auto* DS = llvm::dyn_cast_or_null<clang::DeclStmt>(S->getInit());
               DS && DS->isSingleDecl()) {
                name = ast::identifier_of(llvm::cast<clang::NamedDecl>(*DS->getSingleDecl()));
            } else {
                name = ast::summarize_expr(S->getCond());
            }
            builder.mark_block_end(S->getBody(), "for", name);
        }
        return true;
    }

    bool VisitCXXForRangeStmt(clang::CXXForRangeStmt* S) {
        if(options.block_end) {
            builder.mark_block_end(S->getBody(), "for", ast::identifier_of(*S->getLoopVariable()));
        }
        return true;
    }

    bool VisitWhileStmt(clang::WhileStmt* S) {
        if(options.block_end) {
            builder.mark_block_end(S->getBody(), "while", ast::summarize_expr(S->getCond()));
        }
        return true;
    }

    bool VisitSwitchStmt(clang::SwitchStmt* S) {
        if(options.block_end) {
            builder.mark_block_end(S->getBody(), "switch", ast::summarize_expr(S->getCond()));
        }
        return true;
    }

    bool VisitIfStmt(clang::IfStmt* S) {
        if(options.block_end) {
            if(const auto* else_if = llvm::dyn_cast_or_null<clang::IfStmt>(S->getElse())) {
                else_ifs.insert(else_if);
            }

            // Don't use markBlockEnd: the relevant range is [then.begin, else.end].
            if(const auto* if_end = llvm::dyn_cast<clang::CompoundStmt>(
                   S->getElse() ? S->getElse() : S->getThen())) {
                builder.add_block_end_hint({S->getThen()->getBeginLoc(), if_end->getRBracLoc()},
                                           "if",
                                           else_ifs.contains(S) ? ""
                                                                : ast::summarize_expr(S->getCond()),
                                           "");
            }
        }
        return true;
    }

    bool VisitLambdaExpr(clang::LambdaExpr* expr) {
        clang::FunctionDecl* decl = expr->getCallOperator();
        if(!expr->hasExplicitResultType()) {
            clang::SourceLocation type_hint_loc;
            if(!expr->hasExplicitParameters()) {
                type_hint_loc = expr->getIntroducerRange().getEnd();
            } else if(auto FTL = decl->getFunctionTypeLoc()) {
                type_hint_loc = FTL.getRParenLoc();
            }

            if(type_hint_loc.isValid()) {
                builder.add_return_type_hint(decl, type_hint_loc);
            }
        }
        return true;
    }

    bool VisitInitListExpr(clang::InitListExpr* expr) {
        // We receive the syntactic form here (shouldVisitImplicitCode() is false).
        // This is the one we will ultimately attach designators to.
        // It may have subobject initializers inlined without braces. The *semantic*
        // form of the init-list has nested init-lists for these.
        // getUnwrittenDesignators will look at the semantic form to determine the
        // labels.
        assert(expr->isSyntacticForm() && "RAV should not visit implicit code!");
        if(!options.designators) {
            return true;
        }

        if(expr->isIdiomaticZeroInitializer(unit.lang_options())) {
            return true;
        }

        /// FIXME:
        // llvm::DenseMap<SourceLocation, std::string> Designators =
        //     tidy::utils::getUnwrittenDesignators(Syn);
        // for(const Expr* Init: Syn->inits()) {
        //     if(llvm::isa<DesignatedInitExpr>(Init))
        //         continue;
        //     auto It = Designators.find(Init->getBeginLoc());
        //     if(It != Designators.end() && !isPrecededByParamNameComment(Init, It->second))
        //         addDesignatorHint(Init->getSourceRange(), It->second);
        // }
        return true;
    }

    bool VisitTypeLoc(clang::TypeLoc TL) {
        if(const auto* DT = llvm::dyn_cast<clang::DecltypeType>(TL.getType()))
            if(clang::QualType UT = DT->getUnderlyingType(); !UT->isDependentType())
                builder.add_type_hint(TL.getSourceRange(), UT, ": ");
        return true;
    }

    // FIXME: Handle RecoveryExpr to try to hint some invalid calls.

private:
    Builder& builder;
    CompilationUnit& unit;
    const config::InlayHintsOptions& options;

    // If/else chains are tricky.
    //   if (cond1) {
    //   } else if (cond2) {
    //   } // mark as "cond1" or "cond2"?
    // For now, the answer is neither, just mark as "if".
    // The ElseIf is a different IfStmt that doesn't know about the outer one.
    llvm::DenseSet<const clang::IfStmt*> else_ifs;  // not eligible for names
};

}  // namespace

auto inlay_hints(CompilationUnit& unit,
                 LocalSourceRange target,
                 const config::InlayHintsOptions& options) -> std::vector<InlayHint> {
    std::vector<InlayHint> hints;

    Builder builder(hints, unit, target, options);
    Visitor visitor(builder, unit, target, options);
    visitor.TraverseDecl(unit.tu());

    ranges::sort(hints, refl::less);
    auto sub_range = ranges::unique(hints, refl::equal);
    hints.erase(sub_range.begin(), sub_range.end());

    return hints;
}

}  // namespace clice::feature
