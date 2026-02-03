#include "Feature/Hover.h"

#include "AST/Selection.h"
#include "AST/Semantic.h"
#include "AST/Utility.h"
#include "Compiler/CompilationUnit.h"
#include "Index/Shared.h"
#include "Support/Compare.h"
#include "Support/Logging.h"
#include "Support/Ranges.h"

#include "clang/AST/ASTDiagnostic.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/HeuristicResolver.h"

namespace clice::feature {

namespace {

static auto to_proto_range(clang::SourceManager& sm, clang::SourceRange range) -> proto::Range {
    auto range_b = range.getBegin();
    auto range_e = range.getEnd();
    auto begin = proto::Position{sm.getSpellingLineNumber(range_b) - 1,
                                 sm.getSpellingColumnNumber(range_b) - 1};
    auto end = proto::Position{sm.getSpellingLineNumber(range_e) - 1,
                               sm.getSpellingColumnNumber(range_e) - 1};
    return {begin, end};
};

// Print type and optionally desuguared type
static std::string print_type(clang::ASTContext& ctx,
                              clang::QualType qt,
                              const clang::PrintingPolicy pp,
                              const config::HoverOptions& opt) {
    std::string ret;
    llvm::raw_string_ostream os(ret);
    while(!qt.isNull() && qt->isDecltypeType()) {
        qt = qt->castAs<clang::DecltypeType>()->getUnderlyingType();
    }
    if(!qt.isNull() && !qt.hasQualifiers() && pp.SuppressTagKeyword) {
        if(auto* tt = llvm::dyn_cast<clang::TagType>(qt.getTypePtr());
           tt && tt->isCanonicalUnqualified()) {
            os << tt->getDecl()->getKindName() << ' ';
        }
    }
    qt.print(os, pp);
    if(qt.isNull() && opt.show_aka) {
        bool should_aka = false;
        auto desugared_ty = clang::desugarForDiagnostic(ctx, qt, should_aka);
        if(should_aka) {
            os << "(a.k.a " << desugared_ty.getAsString(pp) << ")";
        }
    }
    return ret;
}

static std::string print_type(const clang::TemplateTypeParmDecl* TTP) {
    std::string ret = TTP->wasDeclaredWithTypename() ? "typename" : "class";
    if(TTP->isParameterPack()) {
        ret += " ...";
    }
    return ret;
}

static std::string print_type(const clang::NonTypeTemplateParmDecl* NTTP,
                              const clang::PrintingPolicy PP,
                              const config::HoverOptions& opt) {
    std::string ret = print_type(NTTP->getASTContext(), NTTP->getType(), PP, opt);
    if(NTTP->isParameterPack()) {
        ret += " ...";
    }
    return ret;
}

static std::string print_type(const clang::TemplateTemplateParmDecl* TTP,
                              const clang::PrintingPolicy PP,
                              const config::HoverOptions& opt) {
    using namespace clang;
    std::string ret;
    llvm::raw_string_ostream OS(ret);
    OS << "template <";
    llvm::StringRef Sep = "";
    for(const Decl* Param: *TTP->getTemplateParameters()) {
        OS << Sep;
        Sep = ", ";
        if(const auto* TTP = dyn_cast<TemplateTypeParmDecl>(Param))
            OS << print_type(TTP);
        else if(const auto* NTTP = dyn_cast<NonTypeTemplateParmDecl>(Param))
            OS << print_type(NTTP, PP, opt);
        else if(const auto* TTPD = dyn_cast<TemplateTemplateParmDecl>(Param))
            OS << print_type(TTPD, PP, opt);
    }
    // FIXME: TemplateTemplateParameter doesn't store the info on whether this
    // param was a "typename" or "class".
    OS << "> class";
    return ret;
}

static std::vector<HoverItem> get_hover_items(CompilationUnitRef unit,
                                              const clang::NamedDecl* decl,
                                              const config::HoverOptions& opt) {
    clang::ASTContext& ctx = unit.context();
    const auto pp = ctx.getPrintingPolicy();
    std::vector<HoverItem> items;

    auto add_item = [&items](HoverItem::HoverKind kind, std::string&& val) {
        items.emplace_back(kind, val);
    };

    // Add type info
    if(const auto* VD = dyn_cast<clang::ValueDecl>(decl)) {
        add_item(HoverItem::Type, print_type(ctx, VD->getType(), pp, opt));
    } else if(const auto* TTP = dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
        add_item(HoverItem::Type, TTP->wasDeclaredWithTypename() ? "typename" : "class");
    } else if(const auto* TTP = dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
        add_item(HoverItem::Type, print_type(TTP, pp, opt));
    } else if(const auto* VT = dyn_cast<clang::VarTemplateDecl>(decl)) {
        add_item(HoverItem::Type, print_type(ctx, VT->getTemplatedDecl()->getType(), pp, opt));
    } else if(const auto* TN = dyn_cast<clang::TypedefNameDecl>(decl)) {
        add_item(HoverItem::Type,
                 print_type(ctx, TN->getUnderlyingType().getDesugaredType(ctx), pp, opt));
    } else if(const auto* TAT = dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
        add_item(HoverItem::Type,
                 print_type(ctx, TAT->getTemplatedDecl()->getUnderlyingType(), pp, opt));
    }

    if(auto fd = llvm::dyn_cast<clang::FieldDecl>(decl)) {
        // LOG_INFO("Got a field decl");
        const auto record = fd->getParent();
        if(!record->isDependentType()) {
            add_item(HoverItem::Offset, llvm::Twine(ctx.getFieldOffset(fd)).str());
            add_item(HoverItem::Align,
                     llvm::Twine(ctx.getTypeAlignInChars(fd->getType()).getQuantity()).str());
            add_item(HoverItem::Size,
                     llvm::Twine(ctx.getTypeSizeInChars(fd->getType()).getQuantity()).str());
        }

        if(record->isUnion()) {
            add_item(HoverItem::Size,
                     llvm::Twine(ctx.getTypeSizeInChars(fd->getType()).getQuantity()).str());
            add_item(HoverItem::Align,
                     llvm::Twine(ctx.getTypeAlignInChars(fd->getType()).getQuantity()).str());
        }

        if(fd->isBitField()) {
            add_item(HoverItem::BitWidth, llvm::Twine(fd->getBitWidthValue()).str());
        }
    } else if(auto vd = llvm::dyn_cast<clang::VarDecl>(decl)) {
        auto ty = vd->getType();
    }

    return items;
}

static std::vector<HoverItem> get_hover_items(CompilationUnitRef unit,
                                              const clang::TypeLoc* typeloc,
                                              const config::HoverOptions& opt) {
    // TODO: Add items for typeloc
    return {};
}

static std::string get_document(CompilationUnitRef unit,
                                const clang::NamedDecl* decl,
                                config::HoverOptions opt) {
    // TODO: Get comment and strip `/**/` and `//`
    clang::ASTContext& Ctx = unit.context();
    const clang::RawComment* comment = Ctx.getRawCommentForAnyRedecl(decl);
    if(!comment) {
        return "";
    }
    auto raw_string = comment->getFormattedText(Ctx.getSourceManager(), Ctx.getDiagnostics());
    LOG_WARN("Got comment:\n```\n{}\n```\n", raw_string);
    return "";
}

static std::string get_qualifier(CompilationUnitRef unit,
                                 const clang::NamedDecl* decl,
                                 config::HoverOptions opt) {
    std::string result;
    llvm::raw_string_ostream os(result);
    decl->printNestedNameSpecifier(os);
    return result;
}

// Get all source code
static std::string get_source_code(CompilationUnitRef unit, clang::SourceRange range) {
    clang::LangOptions lo;
    auto& sm = unit.context().getSourceManager();
    auto start_loc = sm.getSpellingLoc(range.getBegin());
    auto last_token_loc = sm.getSpellingLoc(range.getEnd());
    auto end_loc = clang::Lexer::getLocForEndOfToken(last_token_loc, 0, sm, lo);
    return std::string{clang::Lexer::getSourceText(
        clang::CharSourceRange::getCharRange(clang::SourceRange{start_loc, end_loc}),
        sm,
        lo)};
}

static clang::TemplateTypeParmTypeLoc getContainedAutoParamType(clang::TypeLoc TL) {
    if(auto QTL = TL.getAs<clang::QualifiedTypeLoc>())
        return getContainedAutoParamType(QTL.getUnqualifiedLoc());
    if(llvm::isa<clang::PointerType, clang::ReferenceType, clang::ParenType>(TL.getTypePtr()))
        return getContainedAutoParamType(TL.getNextTypeLoc());
    if(auto FTL = TL.getAs<clang::FunctionTypeLoc>())
        return getContainedAutoParamType(FTL.getReturnLoc());
    if(auto TTPTL = TL.getAs<clang::TemplateTypeParmTypeLoc>()) {
        if(TTPTL.getTypePtr()->getDecl()->isImplicit())
            return TTPTL;
    }
    return {};
}

template <typename TemplateDeclTy>
static clang::NamedDecl* getOnlyInstantiationImpl(TemplateDeclTy* TD) {
    clang::NamedDecl* Only = nullptr;
    for(auto* Spec: TD->specializations()) {
        if(Spec->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization)
            continue;
        if(Only != nullptr)
            return nullptr;
        Only = Spec;
    }
    return Only;
}

static clang::NamedDecl* getOnlyInstantiation(clang::NamedDecl* TemplatedDecl) {
    if(clang::TemplateDecl* TD = TemplatedDecl->getDescribedTemplate()) {
        if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(TD))
            return getOnlyInstantiationImpl(CTD);
        if(auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(TD))
            return getOnlyInstantiationImpl(FTD);
        if(auto* VTD = llvm::dyn_cast<clang::VarTemplateDecl>(TD))
            return getOnlyInstantiationImpl(VTD);
    }
    return nullptr;
}

/// Computes the deduced type at a given location by visiting the relevant
/// nodes. We use this to display the actual type when hovering over an "auto"
/// keyword or "decltype()" expression.
/// FIXME: This could have been a lot simpler by visiting AutoTypeLocs but it
/// seems that the AutoTypeLocs that can be visited along with their AutoType do
/// not have the deduced type set. Instead, we have to go to the appropriate
/// DeclaratorDecl/FunctionDecl and work our back to the AutoType that does have
/// a deduced type set. The AST should be improved to simplify this scenario.
class DeducedTypeVisitor : public clang::RecursiveASTVisitor<DeducedTypeVisitor> {
    clang::SourceLocation SearchedLocation;
    const clang::HeuristicResolver* Resolver;

public:
    DeducedTypeVisitor(clang::SourceLocation SearchedLocation,
                       const clang::HeuristicResolver* Resolver) :
        SearchedLocation(SearchedLocation), Resolver(Resolver) {}

    // Handle auto initializers:
    //- auto i = 1;
    //- decltype(auto) i = 1;
    //- auto& i = 1;
    //- auto* i = &a;
    bool VisitDeclaratorDecl(clang::DeclaratorDecl* D) {
        if(!D->getTypeSourceInfo() ||
           !D->getTypeSourceInfo()->getTypeLoc().getContainedAutoTypeLoc() ||
           D->getTypeSourceInfo()->getTypeLoc().getContainedAutoTypeLoc().getNameLoc() !=
               SearchedLocation)
            return true;

        if(auto* AT = D->getType()->getContainedAutoType()) {
            if(AT->isUndeducedAutoType()) {
                if(const auto* VD = dyn_cast<clang::VarDecl>(D)) {
                    if(Resolver && VD->hasInit()) {
                        // FIXME:
                        // DeducedType = Resolver->resolveExprToType(VD->getInit());
                        DeducedType = VD->getType();
                        return true;
                    }
                }
            }
            DeducedType = AT->desugar();
        }
        return true;
    }

    // Handle auto return types:
    //- auto foo() {}
    //- auto& foo() {}
    //- auto foo() -> int {}
    //- auto foo() -> decltype(1+1) {}
    //- operator auto() const { return 10; }
    bool VisitFunctionDecl(clang::FunctionDecl* D) {
        if(!D->getTypeSourceInfo())
            return true;
        // Loc of auto in return type (c++14).
        auto CurLoc = D->getReturnTypeSourceRange().getBegin();
        // Loc of "auto" in operator auto()
        if(CurLoc.isInvalid() && isa<clang::CXXConversionDecl>(D))
            CurLoc = D->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
        // Loc of "auto" in function with trailing return type (c++11).
        if(CurLoc.isInvalid())
            CurLoc = D->getSourceRange().getBegin();
        if(CurLoc != SearchedLocation)
            return true;

        const clang::AutoType* AT = D->getReturnType()->getContainedAutoType();
        if(AT && !AT->getDeducedType().isNull()) {
            DeducedType = AT->getDeducedType();
        } else if(auto* DT = dyn_cast<clang::DecltypeType>(D->getReturnType())) {
            // auto in a trailing return type just points to a DecltypeType and
            // getContainedAutoType does not unwrap it.
            if(!DT->getUnderlyingType().isNull())
                DeducedType = DT->getUnderlyingType();
        } else if(!D->getReturnType().isNull()) {
            DeducedType = D->getReturnType();
        }
        return true;
    }

    // Handle non-auto decltype, e.g.:
    // - auto foo() -> decltype(expr) {}
    // - decltype(expr);
    bool VisitDecltypeTypeLoc(clang::DecltypeTypeLoc TL) {
        if(TL.getBeginLoc() != SearchedLocation)
            return true;

        // A DecltypeType's underlying type can be another DecltypeType! E.g.
        //  int I = 0;
        //  decltype(I) J = I;
        //  decltype(J) K = J;
        const clang::DecltypeType* DT = dyn_cast<clang::DecltypeType>(TL.getTypePtr());
        while(DT && !DT->getUnderlyingType().isNull()) {
            DeducedType = DT->getUnderlyingType();
            DT = dyn_cast<clang::DecltypeType>(DeducedType.getTypePtr());
        }
        return true;
    }

    // Handle functions/lambdas with `auto` typed parameters.
    // We deduce the type if there's exactly one instantiation visible.
    bool VisitParmVarDecl(clang::ParmVarDecl* PVD) {
        if(!PVD->getType()->isDependentType())
            return true;
        // 'auto' here does not name an AutoType, but an implicit template param.
        clang::TemplateTypeParmTypeLoc Auto =
            getContainedAutoParamType(PVD->getTypeSourceInfo()->getTypeLoc());
        if(Auto.isNull() || Auto.getNameLoc() != SearchedLocation)
            return true;

        // We expect the TTP to be attached to this function template.
        // Find the template and the param index.
        auto* Templated = llvm::dyn_cast<clang::FunctionDecl>(PVD->getDeclContext());
        if(!Templated)
            return true;
        auto* FTD = Templated->getDescribedFunctionTemplate();
        if(!FTD)
            return true;
        int ParamIndex = paramIndex(*FTD, *Auto.getDecl());
        if(ParamIndex < 0) {
            assert(false && "auto TTP is not from enclosing function?");
            return true;
        }

        // Now find the instantiation and the deduced template type arg.
        auto* Instantiation =
            llvm::dyn_cast_or_null<clang::FunctionDecl>(getOnlyInstantiation(Templated));
        if(!Instantiation)
            return true;
        const auto* Args = Instantiation->getTemplateSpecializationArgs();
        if(Args->size() != FTD->getTemplateParameters()->size())
            return true;  // no weird variadic stuff
        DeducedType = Args->get(ParamIndex).getAsType();
        return true;
    }

    static int paramIndex(const clang::TemplateDecl& TD, clang::NamedDecl& Param) {
        unsigned I = 0;
        for(auto* ND: *TD.getTemplateParameters()) {
            if(&Param == ND)
                return I;
            ++I;
        }
        return -1;
    }

    clang::QualType DeducedType;
};

// FIXME: Do as clangd a more simple way?
static std::optional<clang::QualType> getDeducedType(clang::ASTContext& ASTCtx,
                                                     const clang::HeuristicResolver* Resolver,
                                                     clang::SourceLocation Loc) {
    if(!Loc.isValid()) {
        return {};
    }
    DeducedTypeVisitor V(Loc, Resolver);
    V.TraverseAST(ASTCtx);
    if(V.DeducedType.isNull()) {
        return std::nullopt;
    }
    return V.DeducedType;
}

// TODO: How does clangd put together decl, name, scope and sometimes initialized value?
// ```
// // scope
// <Access specifier> <type> <name> <initialized value>
// ```
static std::string get_source_code(CompilationUnitRef unit,
                                   const clang::NamedDecl* decl,
                                   config::HoverOptions opt) {
    clang::SourceRange range = decl->getSourceRange();
    return get_source_code(unit, range);
}

static std::optional<Hover> hover(CompilationUnitRef unit,
                                  const clang::NamedDecl* decl,
                                  const config::HoverOptions& opt) {
    return Hover{
        .kind = SymbolKind::from(decl),
        .name = ast::name_of(decl),
        .items = get_hover_items(unit, decl, opt),
        .document = get_document(unit, decl, opt),
        .qualifier = get_qualifier(unit, decl, opt),
        .source = get_source_code(unit, decl, opt),
    };
}

static std::optional<Hover> hover(CompilationUnitRef unit,
                                  const clang::QualType& ty,
                                  const config::HoverOptions& opt) {
    // TODO: Hover for type
    // TODO: Add source code
    auto& ctx = unit.context();
    auto pp = ctx.getPrintingPolicy();
    return Hover{.kind = SymbolKind::Type, .name = print_type(ctx, ty, pp, opt)};
}

static std::optional<Hover> hover(CompilationUnitRef unit,
                                  const SelectionTree::Node* node,
                                  const config::HoverOptions& opt) {
    using namespace clang;

#define kind_flag_def(Ty) static constexpr auto Flag##Ty = ASTNodeKind::getFromNodeKind<Ty>()

    kind_flag_def(QualType);
    kind_flag_def(TypeLoc);
    kind_flag_def(Decl);
    kind_flag_def(Stmt);
    kind_flag_def(Type);
    kind_flag_def(AutoTypeLoc);
    kind_flag_def(DecltypeTypeLoc);
    kind_flag_def(OMPClause);
    kind_flag_def(TemplateArgument);
    kind_flag_def(TemplateArgumentLoc);
    kind_flag_def(LambdaCapture);
    kind_flag_def(TemplateName);
    kind_flag_def(NestedNameSpecifierLoc);
    kind_flag_def(Attr);
    kind_flag_def(DeclRefExpr);
    kind_flag_def(ObjCProtocolLoc);

#define is(flag) (Kind.isSame(Flag##flag))

#define is_in_range(LHS, RHS) (!((Kind < Flag##LHS) && is(LHS)) && (Kind < Flag##RHS))

    auto wanted_node = node;
    auto Kind = node->data.getNodeKind();

    // auto and decltype is specially processed
    if(is(AutoTypeLoc) || is(DecltypeTypeLoc)) {
        auto resolver = HeuristicResolver(unit.context());
        if(auto ty = getDeducedType(unit.context(), &resolver, node->source_range().getBegin())) {
            return hover(unit, *ty, opt);
        } else {
            LOG_WARN("Cannot get deduced type");
        }
    }

    if(is(NestedNameSpecifierLoc)) {
        if(auto ns_specifier_loc = node->get<NestedNameSpecifierLoc>()) {
            LOG_WARN("Hit a `NestedNameSpecifierLoc`");
            if(auto ns_specifier = ns_specifier_loc->getNestedNameSpecifier()) {
                auto ns = ns_specifier->getAsNamespace();
                assert(ns);
                std::string name;
                if(!ns->isAnonymousNamespace()) {
                    name = ns->getNameAsString();
                } else {
                    name = "Anonymous";
                }
                return Hover{.kind = SymbolKind::Namespace, .name = name};
            } else {
                LOG_WARN("Cannot get namespace");
            }
        }
    } else if(is_in_range(QualType, TypeLoc)) {
        // Typeloc
        LOG_WARN("Hit a `TypeLoc`");
        if(auto typeloc = node->get<TypeLoc>()) {
            return hover(unit, typeloc->getType(), opt);
        }
    } else if(is_in_range(Decl, Stmt)) {
        // Decl
        LOG_WARN("Hit a `Decl`");
        if(auto decl = node->get<clang::NamedDecl>()) {
            return hover(unit, decl, opt);
        } else {
            LOG_WARN("Not intersted");
        }
    } else if(is(DeclRefExpr)) {
        LOG_WARN("Hit an `DeclRef`, Unhandled");
        if(auto dr = node->get<DeclRefExpr>()) {
            auto vd = dr->getDecl();
            assert(vd);
            return hover(unit, llvm::dyn_cast<NamedDecl>(vd), opt);
        }
    } else if(is_in_range(Attr, ObjCProtocolLoc)) {
        LOG_WARN("Hit an `Attr`, Unhandled");
        // TODO: Attr?
    } else {
        // Not interested
        LOG_WARN("Not interested");
    }

#undef is
#undef is_in_range
#undef kind_flag_def

    return std::nullopt;
}

}  // namespace

std::optional<Hover> hover(CompilationUnitRef unit,
                           std::uint32_t offset,
                           const config::HoverOptions& opt) {
    auto& sm = unit.context().getSourceManager();

    auto src_loc_in_main_file = [&sm, &unit](uint32_t off) -> std::optional<clang::SourceLocation> {
        auto fid = sm.getMainFileID();
        auto buf = sm.getBufferData(fid);
        if(off > buf.size()) {
            return std::nullopt;
        }
        return unit.create_location(fid, off);
    };

    // SpellLoc
    auto loc = src_loc_in_main_file(offset);
    if(!loc.has_value()) {
        return std::nullopt;
    }

    // Handle includsions
    bool linenr_invalid = false;
    unsigned linenr = sm.getPresumedLineNumber(*loc, &linenr_invalid);
    if(linenr_invalid) {
        return std::nullopt;
    }

    // FIXME: Cannot handle pch: cannot find records when compiled with pch
    auto directive = unit.directives()[sm.getMainFileID()];
    for(auto& inclusion: directive.includes) {
        bool invalid = false;
        auto inc_linenr = sm.getPresumedLineNumber(inclusion.location, &invalid);
        if(!invalid && inc_linenr == linenr) {
            auto raw_name = get_source_code(unit, inclusion.filename_range);
            auto file_name = llvm::StringRef{raw_name}.trim("<>\"");
            Hover hi;
            hi.kind = SymbolKind::Directive;
            hi.name = file_name;
            auto dir = sm.getFileEntryForID(inclusion.fid)->tryGetRealPathName();
            hi.source = dir;
            // TODO: Provides symbol
            return hi;
        }
    }

    auto tokens_under_cursor = unit.spelled_tokens_touch(*loc);
    if(tokens_under_cursor.empty()) {
        LOG_WARN("Cannot detect tokens");
        return std::nullopt;
    }
    auto hl_range = tokens_under_cursor.back().range(sm).toCharRange(sm).getAsRange();
    for(auto& token: tokens_under_cursor) {
        if(token.kind() == clang::tok::identifier) {
            for(auto& m: directive.macros) {
                if(token.location() == m.loc) {
                    auto name_range = token.range(sm).toCharRange(sm).getAsRange();
                    auto macro_name = get_source_code(unit, name_range);
                    macro_name.pop_back();
                    Hover hi;
                    hi.kind = SymbolKind::Macro;
                    hi.name = macro_name;
                    auto source = "#define " + get_source_code(unit,
                                                               {m.macro->getDefinitionLoc(),
                                                                m.macro->getDefinitionEndLoc()});
                    if(m.kind == MacroRef::Ref) {
                        if(auto expansion = unit.token_buffer().expansionStartingAt(&token)) {
                            std::string expaned_source;
                            for(const auto& expanded_tok: expansion->Expanded) {
                                expaned_source += expanded_tok.text(sm);
                                // TODO: Format code?
                                // expaned_source += ' ';
                                // TODO: Config field: expansion display size
                            }
                            if(!expaned_source.empty()) {
                                source += "\n\n// Expands to:\n";
                                source += expaned_source;
                                source += '\n';
                            }
                        }
                    }
                    hi.source = source;
                    hi.hl_range = to_proto_range(sm, hl_range);
                    return hi;
                }
            }
        }
    }

    auto tree = SelectionTree::create_right(unit, {offset, offset});
    if(auto node = tree.common_ancestor()) {
        LOG_WARN("Got node: {}", node->kind());
        if(auto info = hover(unit, node, opt)) {
            info->hl_range = to_proto_range(sm, hl_range);
            return info;
        }
        return std::nullopt;
    } else {
        LOG_WARN("Not an ast node");
    }

    return std::nullopt;
}

std::optional<std::string> Hover::get_item_content(HoverItem::HoverKind kind) {
    for(auto& item: this->items) {
        if(item.kind == kind) {
            return item.value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> Hover::display(config::HoverOptions opt) {
    std::string content;
    llvm::raw_string_ostream os(content);
    // TODO: generate markdown
    os << std::format("{}: {}\n", this->kind.name(), this->name);
    os << std::format("Contains {} items\n", this->items.size());
    for(auto& hi: this->items) {
        os << std::format("- {}: {}\n", clice::refl::enum_name(hi.kind), hi.value);
    }
    if(this->document) {
        os << "---\n";
        os << "Document:\n```text\n" << *this->document << "\n```\n";
    }
    if(!this->source.empty()) {
        os << "---\n";
        os << "Source code:\n```cpp\n" << this->source << "\n```\n";
    }
    return os.str();
}

}  // namespace clice::feature
