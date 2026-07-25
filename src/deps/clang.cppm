module;

#include <cstddef>
#include <utility>

#include "clang/AST/ASTConcept.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/ODRHash.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RawCommentList.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Basic/AllDiagnostics.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/OperatorKinds.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/Stack.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/Types.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Token.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/HeuristicResolver.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"
#include "clang/Sema/TreeTransform.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"

export module clang;

export namespace clang {

using ::clang::ASTConsumer;
using ::clang::ASTContext;
using ::clang::ASTRecordLayout;
using ::clang::ASTTemplateArgumentListInfo;
using ::clang::AccessSpecDecl;
using ::clang::ArraySizeModifier;
using ::clang::ArrayType;
using ::clang::ArrayTypeLoc;
using ::clang::Attr;
using ::clang::AttributedStmt;
using ::clang::AttributedTypeLoc;
using ::clang::AutoType;
using ::clang::BO_Assign;
using ::clang::BinaryOperator;
using ::clang::BindingDecl;
using ::clang::BlockPointerType;
using ::clang::BuiltinTemplateDecl;
using ::clang::BuiltinType;
using ::clang::BuiltinTypeLoc;
using ::clang::CK_ArrayToPointerDecay;
using ::clang::CK_DerivedToBase;
using ::clang::CK_FunctionToPointerDecay;
using ::clang::CK_LValueToRValue;
using ::clang::CK_NoOp;
using ::clang::CK_NullToMemberPointer;
using ::clang::CK_NullToPointer;
using ::clang::CK_UncheckedDerivedToBase;
using ::clang::CXXBaseSpecifier;
using ::clang::CXXBoolLiteralExpr;
using ::clang::CXXConstructExpr;
using ::clang::CXXConstructorDecl;
using ::clang::CXXConversionDecl;
using ::clang::CXXCtorInitializer;
using ::clang::CXXDeductionGuideDecl;
using ::clang::CXXDefaultArgExpr;
using ::clang::CXXDeleteExpr;
using ::clang::CXXDependentScopeMemberExpr;
using ::clang::CXXDestructorDecl;
using ::clang::CXXForRangeStmt;
using ::clang::CXXFunctionalCastExpr;
using ::clang::CXXMemberCallExpr;
using ::clang::CXXMethodDecl;
using ::clang::CXXNewExpr;
using ::clang::CXXNullPtrLiteralExpr;
using ::clang::CXXOperatorCallExpr;
using ::clang::CXXRecordDecl;
using ::clang::CXXRewrittenBinaryOperator;
using ::clang::CXXTemporaryObjectExpr;
using ::clang::CXXThisExpr;
using ::clang::CXXUnresolvedConstructExpr;
using ::clang::CallExpr;
using ::clang::CharSourceRange;
using ::clang::ClassTemplateDecl;
using ::clang::ClassTemplatePartialSpecializationDecl;
using ::clang::ClassTemplateSpecializationDecl;
using ::clang::CodeCompleteConsumer;
using ::clang::CodeCompleteOptions;
using ::clang::CodeCompletionAllocator;
using ::clang::CodeCompletionContext;
using ::clang::CodeCompletionResult;
using ::clang::CodeCompletionString;
using ::clang::CodeCompletionTUInfo;
using ::clang::CompilerInstance;
using ::clang::CompilerInvocation;
using ::clang::ComplexType;
using ::clang::CompoundLiteralExpr;
using ::clang::CompoundStmt;
using ::clang::ConceptDecl;
using ::clang::ConceptReference;
using ::clang::ConceptSpecializationExpr;
using ::clang::ConstDeclVisitor;
using ::clang::ConstStmtVisitor;
using ::clang::ConstantArrayType;
using ::clang::ConstantArrayTypeLoc;
using ::clang::CreateInvocationOptions;
using ::clang::Decl;
using ::clang::DeclContext;
using ::clang::DeclGroupRef;
using ::clang::DeclRefExpr;
using ::clang::DeclStmt;
using ::clang::DeclarationName;
using ::clang::DeclaratorDecl;
using ::clang::DecltypeType;
using ::clang::DecltypeTypeLoc;
using ::clang::DecompositionDecl;
using ::clang::DeducedTemplateArgument;
using ::clang::DeducedTemplateSpecializationType;
using ::clang::DeducedType;
using ::clang::DependencyDirectivesGetter;
using ::clang::DependentNameType;
using ::clang::DependentNameTypeLoc;
using ::clang::DependentScopeDeclRefExpr;
using ::clang::DependentTemplateSpecializationType;
using ::clang::DependentTemplateSpecializationTypeLoc;
using ::clang::DependentTemplateStorage;
using ::clang::DesignatedInitExpr;
using ::clang::Diagnostic;
using ::clang::DiagnosticConsumer;
using ::clang::DiagnosticIDs;
using ::clang::DiagnosticOptions;
using ::clang::DiagnosticsEngine;
using ::clang::DynTypedNode;
using ::clang::ElaboratedType;
using ::clang::ElaboratedTypeKeyword;
using ::clang::ElaboratedTypeLoc;
using ::clang::EnumConstantDecl;
using ::clang::EnumDecl;
using ::clang::EnumType;
using ::clang::Expr;
using ::clang::ExprWithCleanups;
using ::clang::ExternalSourceSymbolAttr;
using ::clang::FieldDecl;
using ::clang::FileEntryRef;
using ::clang::FileID;
using ::clang::FileManager;
using ::clang::FinalAttr;
using ::clang::FixedPointLiteral;
using ::clang::FloatingLiteral;
using ::clang::ForStmt;
using ::clang::FrontendAction;
using ::clang::FunctionDecl;
using ::clang::FunctionProtoType;
using ::clang::FunctionProtoTypeLoc;
using ::clang::FunctionTemplateDecl;
using ::clang::FunctionTypeLoc;
using ::clang::GeneratePCHAction;
using ::clang::GenerateReducedModuleInterfaceAction;
using ::clang::GlobalCodeCompletionAllocator;
using ::clang::GotoStmt;
using ::clang::HeuristicResolver;
using ::clang::IdentifierInfo;
using ::clang::IdentifierTable;
using ::clang::IfStmt;
using ::clang::IgnoringDiagConsumer;
using ::clang::ImaginaryLiteral;
using ::clang::ImplicitCastExpr;
using ::clang::ImplicitParamDecl;
using ::clang::ImportDecl;
using ::clang::IndirectFieldDecl;
using ::clang::InitListExpr;
using ::clang::InjectedClassNameType;
using ::clang::InjectedClassNameTypeLoc;
using ::clang::IntegerLiteral;
using ::clang::LabelDecl;
using ::clang::LabelStmt;
using ::clang::LambdaExpr;
using ::clang::LangOptions;
using ::clang::LexEmbedParametersResult;
using ::clang::Lexer;
using ::clang::Linkage;
using ::clang::LinkageSpecDecl;
using ::clang::MSGuidDecl;
using ::clang::MSPropertyDecl;
using ::clang::MacroArgs;
using ::clang::MacroDefinition;
using ::clang::MacroDirective;
using ::clang::MacroInfo;
using ::clang::MaterializeTemporaryExpr;
using ::clang::MemberExpr;
using ::clang::Module;
using ::clang::ModuleIdPath;
using ::clang::MultiplexConsumer;
using ::clang::NamedDecl;
using ::clang::NamespaceAliasDecl;
using ::clang::NamespaceDecl;
using ::clang::NestedNameSpecifier;
using ::clang::NestedNameSpecifierLoc;
using ::clang::NonTypeTemplateParmDecl;
using ::clang::ODRHash;
using ::clang::OO_Call;
using ::clang::OO_Equal;
using ::clang::OO_Subscript;
using ::clang::ObjCCategoryDecl;
using ::clang::ObjCCategoryImplDecl;
using ::clang::ObjCImplementationDecl;
using ::clang::ObjCInterfaceDecl;
using ::clang::ObjCInterfaceType;
using ::clang::ObjCIvarDecl;
using ::clang::ObjCIvarRefExpr;
using ::clang::ObjCMessageExpr;
using ::clang::ObjCMethodDecl;
using ::clang::ObjCObjectPointerType;
using ::clang::ObjCObjectType;
using ::clang::ObjCPropertyDecl;
using ::clang::ObjCPropertyRefExpr;
using ::clang::ObjCProtocolExpr;
using ::clang::ObjCProtocolLoc;
using ::clang::OpaqueValueExpr;
using ::clang::OptionalFileEntryRef;
using ::clang::OverloadExpr;
using ::clang::OverloadableAttr;
using ::clang::OverloadedOperatorKind;
using ::clang::OverrideAttr;
using ::clang::PPCallbacks;
using ::clang::PackExpansionExpr;
using ::clang::PackExpansionType;
using ::clang::ParenType;
using ::clang::ParenTypeLoc;
using ::clang::ParmVarDecl;
using ::clang::PointerType;
using ::clang::PointerTypeLoc;
using ::clang::PragmaIntroducerKind;
using ::clang::PredefinedExpr;
using ::clang::PreprocessOnlyAction;
using ::clang::Preprocessor;
using ::clang::PresumedLoc;
using ::clang::PrintingPolicy;
using ::clang::PseudoObjectExpr;
using ::clang::QualType;
using ::clang::QualifiedTypeLoc;
using ::clang::Qualifiers;
using ::clang::RValueReferenceType;
using ::clang::RawComment;
using ::clang::RecordDecl;
using ::clang::RecursiveASTVisitor;
using ::clang::ReferenceType;
using enum ::clang::RefQualifierKind;
using ::clang::ReferenceTypeLoc;
using ::clang::ReturnStmt;
using ::clang::Sema;
using ::clang::ShowIncludesDestination;
using ::clang::SizeOfPackExpr;
using ::clang::SmallVectorImpl;
using ::clang::SourceLocation;
using ::clang::SourceManager;
using ::clang::SourceRange;
using ::clang::Stmt;
using ::clang::StmtIterator;
using ::clang::StringLiteral;
using ::clang::StringRef;
using ::clang::SubstTemplateTypeParmType;
using ::clang::SwitchStmt;
using ::clang::SyntaxOnlyAction;
using ::clang::TSK_ExplicitInstantiationDeclaration;
using ::clang::TSK_ExplicitInstantiationDefinition;
using ::clang::TSK_ExplicitSpecialization;
using ::clang::TSK_ImplicitInstantiation;
using ::clang::TSK_Undeclared;
using ::clang::TagDecl;
using ::clang::TagType;
using ::clang::TagTypeKind;
using ::clang::TagTypeLoc;
using ::clang::TemplateArgument;
using ::clang::TemplateArgumentList;
using ::clang::TemplateArgumentListInfo;
using ::clang::TemplateArgumentLoc;
using ::clang::TemplateArgumentLocContainerIterator;
using ::clang::TemplateDecl;
using ::clang::TemplateDeductionResult;
using ::clang::TemplateDiffTypes;
using ::clang::TemplateName;
using ::clang::TemplateParamObjectDecl;
using ::clang::TemplateParameterList;
using ::clang::TemplateSpecializationKind;
using ::clang::TemplateSpecializationType;
using ::clang::TemplateSpecializationTypeLoc;
using ::clang::TemplateTemplateParmDecl;
using ::clang::TemplateTypeParmDecl;
using ::clang::TemplateTypeParmType;
using ::clang::TemplateTypeParmTypeLoc;
using ::clang::Token;
using ::clang::TranslationUnitDecl;
using ::clang::TreeTransform;
using ::clang::Type;
using ::clang::TypeAliasTemplateDecl;
using ::clang::TypeConstraint;
using ::clang::TypeDecl;
using ::clang::TypeLoc;
using ::clang::TypeLocBuilder;
using ::clang::TypeVisitor;
using ::clang::TypedefDecl;
using ::clang::TypedefNameDecl;
using ::clang::TypedefType;
using ::clang::TypedefTypeLoc;
using ::clang::UO_Deref;
using ::clang::UnaryOperator;
using ::clang::UnresolvedLookupExpr;
using ::clang::UnresolvedMemberExpr;
using ::clang::UnresolvedUsingType;
using ::clang::UnresolvedUsingTypenameDecl;
using ::clang::UnresolvedUsingValueDecl;
using ::clang::UserDefinedLiteral;
using ::clang::UsingDecl;
using ::clang::UsingDirectiveDecl;
using ::clang::UsingEnumDecl;
using ::clang::UsingShadowDecl;
using ::clang::UsingType;
using ::clang::ValueDecl;
using ::clang::VarDecl;
using ::clang::VarTemplateDecl;
using ::clang::VarTemplatePartialSpecializationDecl;
using ::clang::VarTemplateSpecializationDecl;
using ::clang::VectorType;
using ::clang::WhileStmt;
using ::clang::WrapperFrontendAction;
using ::clang::cast;
using ::clang::cast_if_present;
using ::clang::cast_or_null;
using ::clang::createInvocation;
using ::clang::createVFSFromCompilerInvocation;
using ::clang::desugarForDiagnostic;
using ::clang::dyn_cast;
using ::clang::dyn_cast_if_present;
using ::clang::dyn_cast_or_null;
using ::clang::getAccessSpelling;
using ::clang::getClangFullVersion;
using ::clang::getCompletionComment;
using ::clang::getOperatorSpelling;
using ::clang::isAsciiIdentifierContinue;
using ::clang::isLowercase;
using ::clang::isWhitespace;
using ::clang::isa;
using ::clang::isa_and_nonnull;
using ::clang::isa_and_present;
using ::clang::noteBottomOfStack;
using ::clang::operator!=;
using ::clang::operator&;
using ::clang::operator<;
using ::clang::operator<<;
using ::clang::operator<=;
using ::clang::operator==;
using ::clang::operator>;
using ::clang::operator>=;
using ::clang::operator^;
using ::clang::operator|;
// clang::DesiredStackSize (clang/Basic/Stack.h) has internal linkage, which a
// using-declaration cannot export — re-export it as a copy under a distinct
// name, initialized from the original so the value can never drift. Reading a
// constant-initialized const's value is not an odr-use, so this is not a real
// TU-local exposure ([basic.link]); clang's warning is conservative here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-WTU-local-entity-exposure"
constexpr size_t desired_stack_size = ::clang::DesiredStackSize;
#pragma clang diagnostic pop
using ::clang::printTemplateArgumentList;
using ::clang::scanSourceForDependencyDirectives;

// Every concrete clang::*Type class, generated by the TypeNodes.inc X-macro so
// the list tracks the toolchain automatically (X-macro consumers like the
// decl_of_impl switch in ast_utility cast to each of these).
#define TYPE(Class, Base) using ::clang::Class##Type;
#define ABSTRACT_TYPE(Class, Base)
#include "clang/AST/TypeNodes.inc"

}  // namespace clang

export namespace clang::Builtin {

using ::clang::Builtin::BIaddressof;
using ::clang::Builtin::BIas_const;
using ::clang::Builtin::BIforward;
using ::clang::Builtin::BImove;
using ::clang::Builtin::BImove_if_noexcept;

}  // namespace clang::Builtin

export namespace clang::SrcMgr {

using ::clang::SrcMgr::CharacteristicKind;

}  // namespace clang::SrcMgr


export namespace clang::dependency_directives_scan {

using ::clang::dependency_directives_scan::Directive;
using ::clang::dependency_directives_scan::Token;
using ::clang::dependency_directives_scan::cxx_export_module_decl;
using ::clang::dependency_directives_scan::cxx_module_decl;
using ::clang::dependency_directives_scan::pp___include_macros;
using ::clang::dependency_directives_scan::pp_elif;
using ::clang::dependency_directives_scan::pp_elifdef;
using ::clang::dependency_directives_scan::pp_elifndef;
using ::clang::dependency_directives_scan::pp_else;
using ::clang::dependency_directives_scan::pp_endif;
using ::clang::dependency_directives_scan::pp_if;
using ::clang::dependency_directives_scan::pp_ifdef;
using ::clang::dependency_directives_scan::pp_ifndef;
using ::clang::dependency_directives_scan::pp_include;
using ::clang::dependency_directives_scan::pp_include_next;

}  // namespace clang::dependency_directives_scan

export namespace clang::diag {

using ::clang::diag::Flavor;
using ::clang::diag::Group;
using ::clang::diag::Severity;
using ::clang::diag::err_fe_ast_file_malformed;
using ::clang::diag::kind;

}  // namespace clang::diag

export namespace clang::driver {

using ::clang::driver::BindArchAction;
using ::clang::driver::Command;
using ::clang::driver::Compilation;
using ::clang::driver::Driver;
using ::clang::driver::JobList;

}  // namespace clang::driver

export namespace clang::driver::types {

using ::clang::driver::types::ID;
using ::clang::driver::types::TY_INVALID;
using ::clang::driver::types::isAcceptedByClang;
using ::clang::driver::types::lookupTypeForExtension;

}  // namespace clang::driver::types

export namespace clang::format {

using ::clang::format::DefaultFallbackStyle;
using ::clang::format::DefaultFormatStyle;
using ::clang::format::getLLVMStyle;
using ::clang::format::getStyle;
using ::clang::format::reformat;
using ::clang::format::sortIncludes;

}  // namespace clang::format

export namespace clang::frontend {

using ::clang::frontend::GeneratePCH;
using ::clang::frontend::GenerateReducedModuleInterface;

}  // namespace clang::frontend

export namespace clang::sema {

using ::clang::sema::TemplateDeductionInfo;

}  // namespace clang::sema

export namespace clang::syntax {

using ::clang::syntax::Token;
using ::clang::syntax::TokenBuffer;
using ::clang::syntax::TokenCollector;
using ::clang::syntax::spelledTokensTouching;

}  // namespace clang::syntax

export namespace clang::tok {

using ::clang::tok::TokenKind;
using ::clang::tok::char_constant;
using ::clang::tok::colon;
using ::clang::tok::comment;
using ::clang::tok::eod;
using ::clang::tok::eof;
using ::clang::tok::equal;
using ::clang::tok::getTokenName;
using ::clang::tok::hash;
using ::clang::tok::header_name;
using ::clang::tok::identifier;
using ::clang::tok::kw_auto;
using ::clang::tok::kw_const;
using ::clang::tok::kw_decltype;
using ::clang::tok::kw_restrict;
using ::clang::tok::kw_volatile;
using ::clang::tok::l_brace;
using ::clang::tok::l_paren;
using ::clang::tok::numeric_constant;
using ::clang::tok::period;
using ::clang::tok::pp_include_next;
using ::clang::tok::r_paren;
using ::clang::tok::raw_identifier;
using ::clang::tok::semi;
using ::clang::tok::string_literal;
using ::clang::tok::unknown;
using ::clang::tok::utf16_char_constant;
using ::clang::tok::utf16_string_literal;
using ::clang::tok::utf32_char_constant;
using ::clang::tok::utf32_string_literal;
using ::clang::tok::utf8_char_constant;
using ::clang::tok::utf8_string_literal;
using ::clang::tok::wide_char_constant;
using ::clang::tok::wide_string_literal;

}  // namespace clang::tok

export namespace clang::tooling {

using ::clang::tooling::Range;
using ::clang::tooling::Replacements;
using ::clang::tooling::applyAllReplacements;
using ::clang::tooling::calculateRangesAfterReplacements;

}  // namespace clang::tooling

// Unscoped enum values are no longer visible through unqualified lookup from
// importers (clang 22 fix for #131058). Export the ones we use.
export using enum ::CXAvailabilityKind;

// Adapter shims: clice's structured-binding support for clang::SourceRange.
// The std specializations live in purview (found by reachability); the get
// helper is exported so ADL finds it in every importer.
namespace std {

template <>
struct tuple_size<::clang::SourceRange> : integral_constant<size_t, 2> {};

template <>
struct tuple_element<0, ::clang::SourceRange> {
    using type = ::clang::SourceLocation;
};

template <>
struct tuple_element<1, ::clang::SourceRange> {
    using type = ::clang::SourceLocation;
};

}  // namespace std

export namespace clang {

template <::std::size_t I>
::clang::SourceLocation get(::clang::SourceRange range) {
    if constexpr(I == 0) {
        return range.getBegin();
    } else {
        return range.getEnd();
    }
}  // namespace clang

}  // namespace clang

// Diagnostic ID enumerators: each Diagnostic*Kinds.inc component declares its
// own ANONYMOUS unscoped enum in clang::diag, so the enumerators cannot be
// exported by name lists or a named using-enum. CWG 2621 (DR against C++20)
// allows using-enum through an alias, and decltype of one representative
// enumerator names the anonymous type — one alias + using-enum per component.
export namespace clang::diag {

using ASTDiags = decltype(err_asm_empty_symbolic_operand_name);
using enum ASTDiags;
using CommentDiags = decltype(note_add_deprecation_attr);
using enum CommentDiags;
using CommonDiags = decltype(err_aix_musttail_unsupported);
using enum CommonDiags;
using CrossTUDiags = decltype(err_ctu_error_opening);
using enum CrossTUDiags;
using DriverDiags = decltype(err_analyzer_checker_incompatible_analyzer_option);
using enum DriverDiags;
using FrontendDiags = decltype(err_alias_to_common);
using enum FrontendDiags;
using InstallAPIDiags = decltype(err_allowable_clients_mismatch);
using enum InstallAPIDiags;
using LexDiags = decltype(backslash_newline_space);
using enum LexDiags;
using ParseDiags = decltype(compat_cxx11_enum_fixed_underlying_type);
using enum ParseDiags;
using RefactoringDiags = decltype(err_refactor_code_outside_of_function);
using enum RefactoringDiags;
using SemaDiags = decltype(compat_cxx11_nonclass_type_friend);
using enum SemaDiags;
using SerializationDiags = decltype(err_ast_file_diagopt_mismatch);
using enum SerializationDiags;

}  // namespace clang::diag
