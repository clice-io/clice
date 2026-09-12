#include "semantic/identity.h"

#include <utility>

#include "semantic/expr_hash.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/xxhash.h"
#include "clang/AST/ASTConcept.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Module.h"

namespace clice {

namespace {

/// Seeds every hash; bump it whenever a rule below changes so old hashes
/// can never collide with new ones by accident.
constexpr std::uint8_t identity_scheme = 1;

/// Segment markers. Their numeric values are part of the hash, so append
/// only.
enum class Tag : std::uint8_t {
    Null,
    Cycle,
    Module,
    Context,
    ContextKind,
    Location,
    Path,
    Local,
    TemplateParameter,
    Namespace,
    AnonymousNamespace,
    Record,
    Union,
    Enum,
    Lambda,
    TypedefName,
    Typedef,
    Enumerator,
    Field,
    IndirectField,
    Variable,
    Function,
    FunctionSpecialization,
    Constructor,
    Destructor,
    Conversion,
    Operator,
    Literal,
    DeductionGuide,
    Identifier,
    Concept,
    BuiltinTemplate,
    Using,
    UnresolvedUsing,
    UsingEnum,
    UsingDirective,
    NamespaceAlias,
    Guid,
    TemplateParamObject,
    UnnamedGlobalConstant,
    Other,
    Macro,
    Type,
    Expr,
    NestedNameSpecifier,
    TemplateName,
    TemplateArgument,
    TemplateHead,
    Name,
    Value,
};

bool has_c_linkage(const clang::Decl* decl) {
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->getLanguageLinkage() == clang::CLanguageLinkage;
    }
    if(auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return variable->getLanguageLinkage() == clang::CLanguageLinkage;
    }
    return false;
}

bool is_template_parameter(const clang::Decl* decl) {
    return llvm::isa<clang::TemplateTypeParmDecl,
                     clang::NonTypeTemplateParmDecl,
                     clang::TemplateTemplateParmDecl>(decl);
}

}  // namespace

/// A byte stream with a fixed layout: every integer is eight little-endian
/// bytes, every string is its length followed by its bytes, so equal
/// sequences of additions produce equal hashes on every platform.
class EntityTable::Hasher {
public:
    Hasher() {
        bytes.push_back(static_cast<char>(identity_scheme));
    }

    void add(Tag tag) {
        bytes.push_back(static_cast<char>(tag));
    }

    void add(std::uint64_t value) {
        for(unsigned i = 0; i < 8; i += 1) {
            bytes.push_back(static_cast<char>(value >> (8 * i)));
        }
    }

    void add(llvm::StringRef text) {
        add(static_cast<std::uint64_t>(text.size()));
        bytes.append(text.begin(), text.end());
    }

    void add(const llvm::APInt& value) {
        add(static_cast<std::uint64_t>(value.getBitWidth()));
        for(unsigned i = 0; i < value.getNumWords(); i += 1) {
            add(value.getRawData()[i]);
        }
    }

    void add(const llvm::APFloat& value) {
        add(static_cast<std::uint64_t>(llvm::APFloatBase::SemanticsToEnum(value.getSemantics())));
        add(value.bitcastToAPInt());
    }

    std::uint64_t finish() const {
        return llvm::xxh3_64bits(llvm::StringRef(bytes.data(), bytes.size()));
    }

private:
    llvm::SmallVector<char, 256> bytes;
};

/// The stable leaves of an expression profile: what ExprHasher hands out
/// as pointers upstream becomes an entity, a type hash or a string here.
/// The template-parameter and function-parameter cases mirror
/// StmtProfilerWithPointers so the walk's structure stays identical.
class EntityTable::Leaves : public ExprHashLeaves {
public:
    Leaves(EntityTable& table, llvm::FoldingSetNodeID& id) : table(table), id(id) {}

    void add_decl(const clang::Decl* decl) override {
        id.AddInteger(decl ? decl->getKind() : 0);
        if(!decl) {
            return;
        }

        if(auto* parameter = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(decl)) {
            id.AddInteger(parameter->getDepth());
            id.AddInteger(parameter->getIndex());
            id.AddBoolean(parameter->isParameterPack());
            auto& context = table.unit.context();
            id.AddInteger(table.type_hash(context.getUnconstrainedType(parameter->getType())));
            return;
        }

        if(auto* parameter = llvm::dyn_cast<clang::ParmVarDecl>(decl)) {
            id.AddInteger(table.type_hash(parameter->getType()));
            id.AddInteger(parameter->getFunctionScopeDepth());
            id.AddInteger(parameter->getFunctionScopeIndex());
            return;
        }

        if(auto* parameter = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            id.AddInteger(parameter->getDepth());
            id.AddInteger(parameter->getIndex());
            id.AddBoolean(parameter->isParameterPack());
            return;
        }

        if(auto* parameter = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
            id.AddInteger(parameter->getDepth());
            id.AddInteger(parameter->getIndex());
            id.AddBoolean(parameter->isParameterPack());
            return;
        }

        /// A declaration statement inside a statement expression can hold
        /// unnamed declarations (static_assert, empty declarations); their
        /// kind above is all they have.
        if(auto* named = llvm::dyn_cast<clang::NamedDecl>(decl)) {
            id.AddInteger(table.entity(named));
        }
    }

    void add_type(clang::QualType type) override {
        id.AddInteger(table.type_hash(type));
    }

    void add_name(clang::DeclarationName name, bool treat_as_decl) override {
        id.AddBoolean(treat_as_decl);
        Hasher hasher;
        table.add_declaration_name(hasher, name);
        id.AddInteger(hasher.finish());
    }

    void add_identifier(const clang::IdentifierInfo* identifier) override {
        id.AddBoolean(identifier != nullptr);
        if(identifier) {
            id.AddString(identifier->getName());
        }
    }

    void add_nested_name_specifier(clang::NestedNameSpecifier specifier) override {
        Hasher hasher;
        table.add_nested_name_specifier(hasher, specifier);
        id.AddInteger(hasher.finish());
    }

    void add_template_name(clang::TemplateName name) override {
        Hasher hasher;
        table.add_template_name(hasher, name);
        id.AddInteger(hasher.finish());
    }

    void add_structural_value(clang::QualType type, const clang::APValue& value) override {
        id.AddInteger(table.type_hash(type));
        Hasher hasher;
        table.add_value(hasher, value);
        id.AddInteger(hasher.finish());
    }

private:
    EntityTable& table;
    llvm::FoldingSetNodeID& id;
};

std::uint64_t EntityTable::entity(const clang::NamedDecl* decl) {
    assert(decl);

    /// A template and the declaration it describes are one entity, as are
    /// a using-declaration's shadow and its target.
    if(auto* templated = llvm::dyn_cast<clang::RedeclarableTemplateDecl>(decl)) {
        return entity(templated->getTemplatedDecl());
    }
    if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl)) {
        return entity(shadow->getTargetDecl());
    }

    /// Only the first declaration's spelling agrees across translation
    /// units: a later one may carry a deduced return type or an inherited
    /// calling convention.
    decl = llvm::cast<clang::NamedDecl>(decl->getCanonicalDecl());

    if(auto it = entities.find(decl); it != entities.end()) {
        return it->second;
    }

    /// Identity only depends on declarations that precede the declaration
    /// in well-formed code, so a cycle can only come from error recovery;
    /// it degrades to the kind and name rather than recursing forever.
    if(!in_progress.insert(decl).second) {
        Hasher hasher;
        hasher.add(Tag::Cycle);
        hasher.add(static_cast<std::uint64_t>(decl->getKind()));
        hasher.add(decl->getName());
        return hasher.finish();
    }

    Hasher hasher;
    add_context(hasher, decl);
    add_self(hasher, decl);
    auto result = hasher.finish();

    in_progress.erase(decl);
    entities[decl] = result;
    return result;
}

std::uint64_t EntityTable::entity(llvm::StringRef name, clang::SourceLocation definition) {
    Hasher hasher;
    hasher.add(Tag::Macro);
    hasher.add(name);
    add_path(hasher, definition);
    return hasher.finish();
}

std::uint64_t EntityTable::type_hash(clang::QualType type) {
    if(type.isNull()) {
        return 0;
    }

    auto canonical = unit.context().getCanonicalType(type);
    auto key = canonical.getAsOpaquePtr();
    if(auto it = types.find(key); it != types.end()) {
        return it->second;
    }

    Hasher hasher;
    hasher.add(Tag::Type);
    auto split = canonical.split();
    hasher.add(static_cast<std::uint64_t>(split.Quals.getAsOpaqueValue()));
    hasher.add(static_cast<std::uint64_t>(split.Ty->getTypeClass()));

    using namespace clang;
    const Type* T = split.Ty;
    switch(T->getTypeClass()) {
        case Type::Builtin: {
            hasher.add(static_cast<std::uint64_t>(cast<BuiltinType>(T)->getKind()));
            break;
        }

        case Type::Complex: {
            add_type(hasher, cast<ComplexType>(T)->getElementType());
            break;
        }

        case Type::Pointer: {
            add_type(hasher, cast<PointerType>(T)->getPointeeType());
            break;
        }

        case Type::BlockPointer: {
            add_type(hasher, cast<BlockPointerType>(T)->getPointeeType());
            break;
        }

        case Type::LValueReference:
        case Type::RValueReference: {
            add_type(hasher, cast<ReferenceType>(T)->getPointeeType());
            break;
        }

        case Type::MemberPointer: {
            auto* member = cast<MemberPointerType>(T);
            add_type(hasher, member->getPointeeType());
            add_nested_name_specifier(hasher, member->getQualifier());
            break;
        }

        case Type::ConstantArray:
        case Type::ArrayParameter: {
            auto* array = cast<ConstantArrayType>(T);
            add_type(hasher, array->getElementType());
            hasher.add(array->getSize());
            hasher.add(static_cast<std::uint64_t>(array->getSizeModifier()));
            hasher.add(static_cast<std::uint64_t>(array->getIndexTypeCVRQualifiers()));
            break;
        }

        case Type::IncompleteArray: {
            auto* array = cast<IncompleteArrayType>(T);
            add_type(hasher, array->getElementType());
            hasher.add(static_cast<std::uint64_t>(array->getSizeModifier()));
            hasher.add(static_cast<std::uint64_t>(array->getIndexTypeCVRQualifiers()));
            break;
        }

        case Type::VariableArray: {
            auto* array = cast<VariableArrayType>(T);
            add_type(hasher, array->getElementType());
            add_expr(hasher, array->getSizeExpr());
            hasher.add(static_cast<std::uint64_t>(array->getSizeModifier()));
            break;
        }

        case Type::DependentSizedArray: {
            auto* array = cast<DependentSizedArrayType>(T);
            add_type(hasher, array->getElementType());
            add_expr(hasher, array->getSizeExpr());
            hasher.add(static_cast<std::uint64_t>(array->getSizeModifier()));
            break;
        }

        case Type::DependentSizedExtVector: {
            auto* vector = cast<DependentSizedExtVectorType>(T);
            add_type(hasher, vector->getElementType());
            add_expr(hasher, vector->getSizeExpr());
            break;
        }

        case Type::DependentVector: {
            auto* vector = cast<DependentVectorType>(T);
            add_type(hasher, vector->getElementType());
            add_expr(hasher, vector->getSizeExpr());
            hasher.add(static_cast<std::uint64_t>(vector->getVectorKind()));
            break;
        }

        case Type::Vector:
        case Type::ExtVector: {
            auto* vector = cast<VectorType>(T);
            add_type(hasher, vector->getElementType());
            hasher.add(static_cast<std::uint64_t>(vector->getNumElements()));
            hasher.add(static_cast<std::uint64_t>(vector->getVectorKind()));
            break;
        }

        case Type::ConstantMatrix: {
            auto* matrix = cast<ConstantMatrixType>(T);
            add_type(hasher, matrix->getElementType());
            hasher.add(static_cast<std::uint64_t>(matrix->getNumRows()));
            hasher.add(static_cast<std::uint64_t>(matrix->getNumColumns()));
            break;
        }

        case Type::DependentSizedMatrix: {
            auto* matrix = cast<DependentSizedMatrixType>(T);
            add_type(hasher, matrix->getElementType());
            add_expr(hasher, matrix->getRowExpr());
            add_expr(hasher, matrix->getColumnExpr());
            break;
        }

        case Type::FunctionNoProto:
        case Type::FunctionProto: {
            auto* function = cast<FunctionType>(T);
            add_type(hasher, function->getReturnType());
            auto info = function->getExtInfo();
            hasher.add(static_cast<std::uint64_t>(info.getCC()));
            hasher.add(static_cast<std::uint64_t>(info.getNoReturn()));
            hasher.add(static_cast<std::uint64_t>(info.getProducesResult()));
            hasher.add(static_cast<std::uint64_t>(info.getRegParm()));
            hasher.add(static_cast<std::uint64_t>(info.getNoCallerSavedRegs()));
            hasher.add(static_cast<std::uint64_t>(info.getNoCfCheck()));
            hasher.add(static_cast<std::uint64_t>(info.getCmseNSCall()));

            auto* proto = dyn_cast<FunctionProtoType>(T);
            if(!proto) {
                break;
            }
            hasher.add(static_cast<std::uint64_t>(proto->getNumParams()));
            for(auto parameter: proto->getParamTypes()) {
                add_type(hasher, parameter);
            }
            hasher.add(static_cast<std::uint64_t>(proto->isVariadic()));
            hasher.add(static_cast<std::uint64_t>(proto->getMethodQuals().getAsOpaqueValue()));
            hasher.add(static_cast<std::uint64_t>(proto->getRefQualifier()));
            hasher.add(static_cast<std::uint64_t>(proto->getExceptionSpecType()));
            for(auto exception: proto->exceptions()) {
                add_type(hasher, exception);
            }
            if(proto->getExceptionSpecType() == EST_DependentNoexcept) {
                add_expr(hasher, proto->getNoexceptExpr());
            }
            if(proto->hasExtParameterInfos()) {
                for(auto extra: proto->getExtParameterInfos()) {
                    hasher.add(static_cast<std::uint64_t>(extra.getOpaqueValue()));
                }
            }
            auto effects = proto->getFunctionEffects();
            for(auto effect: effects.effects()) {
                hasher.add(static_cast<std::uint64_t>(effect.kind()));
            }
            for(auto condition: effects.conditions()) {
                add_expr(hasher, condition.getCondition());
            }
            break;
        }

        case Type::UnresolvedUsing: {
            hasher.add(entity(cast<UnresolvedUsingType>(T)->getDecl()));
            break;
        }

        case Type::TypeOfExpr: {
            auto* type_of = cast<TypeOfExprType>(T);
            add_expr(hasher, type_of->getUnderlyingExpr());
            hasher.add(static_cast<std::uint64_t>(type_of->getKind()));
            break;
        }

        case Type::TypeOf: {
            auto* type_of = cast<TypeOfType>(T);
            add_type(hasher, type_of->getUnmodifiedType());
            hasher.add(static_cast<std::uint64_t>(type_of->getKind()));
            break;
        }

        case Type::Decltype: {
            add_expr(hasher, cast<DecltypeType>(T)->getUnderlyingExpr());
            break;
        }

        case Type::UnaryTransform: {
            auto* transform = cast<UnaryTransformType>(T);
            add_type(hasher, transform->getBaseType());
            hasher.add(static_cast<std::uint64_t>(transform->getUTTKind()));
            break;
        }

        case Type::PackIndexing: {
            auto* indexing = cast<PackIndexingType>(T);
            add_type(hasher, indexing->getPattern());
            add_expr(hasher, indexing->getIndexExpr());
            hasher.add(static_cast<std::uint64_t>(indexing->isFullySubstituted()));
            break;
        }

        case Type::Record:
        case Type::Enum:
        case Type::InjectedClassName: {
            hasher.add(entity(cast<TagType>(T)->getDecl()));
            break;
        }

        case Type::TemplateTypeParm: {
            auto* parameter = cast<TemplateTypeParmType>(T);
            hasher.add(static_cast<std::uint64_t>(parameter->getDepth()));
            hasher.add(static_cast<std::uint64_t>(parameter->getIndex()));
            hasher.add(static_cast<std::uint64_t>(parameter->isParameterPack()));
            break;
        }

        case Type::SubstTemplateTypeParmPack:
        case Type::SubstBuiltinTemplatePack: {
            add_template_argument(hasher, cast<SubstPackType>(T)->getArgumentPack());
            break;
        }

        case Type::TemplateSpecialization: {
            auto* specialization = cast<TemplateSpecializationType>(T);
            add_template_name(hasher, specialization->getTemplateName());
            add_template_arguments(hasher, specialization->template_arguments());
            break;
        }

        case Type::DependentName: {
            auto* dependent = cast<DependentNameType>(T);
            add_nested_name_specifier(hasher, dependent->getQualifier());
            hasher.add(dependent->getIdentifier()->getName());
            break;
        }

        case Type::PackExpansion: {
            auto* expansion = cast<PackExpansionType>(T);
            add_type(hasher, expansion->getPattern());
            hasher.add(static_cast<std::uint64_t>(expansion->getNumExpansions().toInternalRepresentation()));
            break;
        }

        case Type::Auto: {
            auto* deduced = cast<AutoType>(T);
            hasher.add(static_cast<std::uint64_t>(deduced->getKeyword()));
            hasher.add(static_cast<std::uint64_t>(deduced->isConstrained()));
            if(deduced->isConstrained()) {
                hasher.add(entity(deduced->getTypeConstraintConcept()));
                add_template_arguments(hasher, deduced->getTypeConstraintArguments());
            }
            add_type(hasher, deduced->getDeducedType());
            break;
        }

        case Type::DeducedTemplateSpecialization: {
            auto* deduced = cast<DeducedTemplateSpecializationType>(T);
            add_template_name(hasher, deduced->getTemplateName());
            add_type(hasher, deduced->getDeducedType());
            break;
        }

        case Type::Atomic: {
            add_type(hasher, cast<AtomicType>(T)->getValueType());
            break;
        }

        case Type::Pipe: {
            auto* pipe = cast<PipeType>(T);
            add_type(hasher, pipe->getElementType());
            hasher.add(static_cast<std::uint64_t>(pipe->isReadOnly()));
            break;
        }

        case Type::BitInt: {
            auto* bits = cast<BitIntType>(T);
            hasher.add(static_cast<std::uint64_t>(bits->isUnsigned()));
            hasher.add(static_cast<std::uint64_t>(bits->getNumBits()));
            break;
        }

        case Type::DependentBitInt: {
            auto* bits = cast<DependentBitIntType>(T);
            hasher.add(static_cast<std::uint64_t>(bits->isUnsigned()));
            add_expr(hasher, bits->getNumBitsExpr());
            break;
        }

        case Type::DependentAddressSpace: {
            auto* space = cast<DependentAddressSpaceType>(T);
            add_type(hasher, space->getPointeeType());
            add_expr(hasher, space->getAddrSpaceExpr());
            break;
        }

        /// Objective-C and HLSL types never carry an anonymous entity, so
        /// their printed form is stable enough.
        case Type::ObjCObject:
        case Type::ObjCInterface:
        case Type::ObjCObjectPointer:
        case Type::HLSLAttributedResource:
        case Type::HLSLInlineSpirv: {
            hasher.add(clang::QualType(canonical).getAsString());
            break;
        }

        /// Never canonical.
        case Type::Adjusted:
        case Type::Decayed:
        case Type::Attributed:
        case Type::BTFTagAttributed:
        case Type::CountAttributed:
        case Type::MacroQualified:
        case Type::ObjCTypeParam:
        case Type::Paren:
        case Type::PredefinedSugar:
        case Type::SubstTemplateTypeParm:
        case Type::Typedef:
        case Type::Using: {
            std::unreachable();
        }
    }

    auto result = hasher.finish();
    types[key] = result;
    return result;
}

std::uint64_t EntityTable::expr_hash(const clang::Expr* expr) {
    assert(expr);

    if(auto it = exprs.find(expr); it != exprs.end()) {
        return it->second;
    }

    llvm::FoldingSetNodeID id;
    Leaves leaves(*this, id);
    ExprHasher(id, unit.context(), leaves).Visit(expr);

    llvm::BumpPtrAllocator scratch;
    auto data = id.Intern(scratch);
    auto result = llvm::xxh3_64bits(
        llvm::StringRef(reinterpret_cast<const char*>(data.getData()),
                        data.getSize() * sizeof(unsigned)));
    exprs[expr] = result;
    return result;
}

void EntityTable::add_context(Hasher& hasher, const clang::Decl* decl) {
    if(auto* module = decl->getOwningModuleForLinkage()) {
        hasher.add(Tag::Module);
        hasher.add(module->getFullModuleName());
    }

    /// C linkage names one entity across every namespace.
    if(has_c_linkage(decl)) {
        return;
    }

    const clang::DeclContext* context = decl->getDeclContext();
    while(context && !context->isTranslationUnit()) {
        if(llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(context)) {
            context = context->getParent();
            continue;
        }
        if(auto* named = llvm::dyn_cast<clang::NamedDecl>(context)) {
            hasher.add(Tag::Context);
            hasher.add(entity(named));
            return;
        }
        /// Requires-expression bodies, blocks and captured statements have
        /// no name; their kind keeps the chain distinct.
        hasher.add(Tag::ContextKind);
        hasher.add(static_cast<std::uint64_t>(llvm::cast<clang::Decl>(context)->getKind()));
        context = context->getParent();
    }
}

void EntityTable::add_self(Hasher& hasher, const clang::NamedDecl* decl) {
    using namespace clang;

    if(is_template_parameter(decl)) {
        hasher.add(Tag::TemplateParameter);
        hasher.add(static_cast<std::uint64_t>(decl->getKind()));
        add_location(hasher, decl->getLocation());
        return;
    }

    /// Block scope: shadowing makes the name insufficient, and two
    /// included files can put the same offset in the same function.
    if(decl->getParentFunctionOrMethod() || isa<ParmVarDecl>(decl)) {
        hasher.add(Tag::Local);
        hasher.add(static_cast<std::uint64_t>(decl->getKind()));
        hasher.add(decl->getName());
        add_location(hasher, decl->getLocation());
        return;
    }

    if(auto* ns = dyn_cast<NamespaceDecl>(decl)) {
        if(ns->isAnonymousNamespace()) {
            hasher.add(Tag::AnonymousNamespace);
        } else {
            hasher.add(Tag::Namespace);
            hasher.add(ns->getName());
            hasher.add(static_cast<std::uint64_t>(ns->isInline()));
        }
        return;
    }

    if(auto* tag = dyn_cast<TagDecl>(decl)) {
        hasher.add(tag->isUnion() ? Tag::Union : tag->isEnum() ? Tag::Enum : Tag::Record);
        auto* record = dyn_cast<CXXRecordDecl>(tag);
        if(record && record->isLambda()) {
            hasher.add(Tag::Lambda);
            add_location(hasher, tag->getLocation());
        } else if(tag->getDeclName().isEmpty()) {
            if(auto* typedef_name = tag->getTypedefNameForAnonDecl()) {
                hasher.add(Tag::TypedefName);
                hasher.add(typedef_name->getName());
            } else {
                add_location(hasher, tag->getLocation());
            }
        } else {
            hasher.add(tag->getName());
        }

        if(auto* partial = dyn_cast<ClassTemplatePartialSpecializationDecl>(tag)) {
            add_template_arguments(hasher, partial->getTemplateArgs().asArray());
            add_template_head(hasher, partial->getTemplateParameters());
        } else if(auto* specialization = dyn_cast<ClassTemplateSpecializationDecl>(tag)) {
            add_template_arguments(hasher, specialization->getTemplateArgs().asArray());
        } else if(record) {
            if(auto* described = record->getDescribedClassTemplate()) {
                add_template_head(hasher, described->getTemplateParameters());
            }
        }
    } else if(auto* alias = dyn_cast<TypedefNameDecl>(decl)) {
        hasher.add(Tag::Typedef);
        hasher.add(alias->getName());
        if(auto* type_alias = dyn_cast<TypeAliasDecl>(alias)) {
            if(auto* described = type_alias->getDescribedAliasTemplate()) {
                add_template_head(hasher, described->getTemplateParameters());
            }
        }
    } else if(isa<EnumConstantDecl>(decl)) {
        hasher.add(Tag::Enumerator);
        hasher.add(decl->getName());
    } else if(isa<FieldDecl, MSPropertyDecl>(decl)) {
        hasher.add(Tag::Field);
        if(decl->getDeclName().isEmpty()) {
            add_location(hasher, decl->getLocation());
        } else {
            hasher.add(decl->getName());
        }
    } else if(isa<IndirectFieldDecl>(decl)) {
        hasher.add(Tag::IndirectField);
        hasher.add(decl->getName());
    } else if(auto* variable = dyn_cast<VarDecl>(decl)) {
        hasher.add(Tag::Variable);
        if(variable->getDeclName().isEmpty()) {
            add_location(hasher, variable->getLocation());
        } else {
            hasher.add(variable->getName());
        }
        if(auto* partial = dyn_cast<VarTemplatePartialSpecializationDecl>(variable)) {
            add_template_arguments(hasher, partial->getTemplateArgs().asArray());
            add_template_head(hasher, partial->getTemplateParameters());
        } else if(auto* specialization = dyn_cast<VarTemplateSpecializationDecl>(variable)) {
            add_template_arguments(hasher, specialization->getTemplateArgs().asArray());
        } else if(auto* described = variable->getDescribedVarTemplate()) {
            add_template_head(hasher, described->getTemplateParameters());
        }
    } else if(isa<BindingDecl>(decl)) {
        hasher.add(Tag::Variable);
        hasher.add(decl->getName());
    } else if(auto* function = dyn_cast<FunctionDecl>(decl)) {
        add_function(hasher, function);
    } else if(isa<ConceptDecl>(decl)) {
        hasher.add(Tag::Concept);
        hasher.add(decl->getName());
    } else if(isa<BuiltinTemplateDecl>(decl)) {
        hasher.add(Tag::BuiltinTemplate);
        hasher.add(decl->getName());
    } else if(auto* using_decl = dyn_cast<UsingDecl>(decl)) {
        hasher.add(Tag::Using);
        add_nested_name_specifier(hasher, using_decl->getQualifier());
        add_declaration_name(hasher, using_decl->getDeclName());
    } else if(auto* unresolved = dyn_cast<UnresolvedUsingValueDecl>(decl)) {
        hasher.add(Tag::UnresolvedUsing);
        add_nested_name_specifier(hasher, unresolved->getQualifier());
        add_declaration_name(hasher, unresolved->getDeclName());
    } else if(auto* unresolved = dyn_cast<UnresolvedUsingTypenameDecl>(decl)) {
        hasher.add(Tag::UnresolvedUsing);
        add_nested_name_specifier(hasher, unresolved->getQualifier());
        add_declaration_name(hasher, unresolved->getDeclName());
    } else if(auto* using_enum = dyn_cast<UsingEnumDecl>(decl)) {
        hasher.add(Tag::UsingEnum);
        hasher.add(entity(using_enum->getEnumDecl()));
    } else if(auto* directive = dyn_cast<UsingDirectiveDecl>(decl)) {
        hasher.add(Tag::UsingDirective);
        hasher.add(entity(directive->getNominatedNamespace()));
    } else if(isa<NamespaceAliasDecl>(decl)) {
        hasher.add(Tag::NamespaceAlias);
        hasher.add(decl->getName());
    } else if(auto* guid = dyn_cast<MSGuidDecl>(decl)) {
        hasher.add(Tag::Guid);
        auto parts = guid->getParts();
        hasher.add(static_cast<std::uint64_t>(parts.Part1));
        hasher.add(static_cast<std::uint64_t>(parts.Part2));
        hasher.add(static_cast<std::uint64_t>(parts.Part3));
        for(auto byte: parts.Part4And5) {
            hasher.add(static_cast<std::uint64_t>(byte));
        }
    } else if(auto* object = dyn_cast<TemplateParamObjectDecl>(decl)) {
        hasher.add(Tag::TemplateParamObject);
        add_type(hasher, object->getType());
        add_value(hasher, object->getValue());
    } else if(auto* constant = dyn_cast<UnnamedGlobalConstantDecl>(decl)) {
        hasher.add(Tag::UnnamedGlobalConstant);
        add_type(hasher, constant->getType());
        add_value(hasher, constant->getValue());
    } else {
        hasher.add(Tag::Other);
        hasher.add(static_cast<std::uint64_t>(decl->getKind()));
        hasher.add(decl->getName());
    }

    /// A namespace-scope declaration nothing outside the translation unit
    /// can see is one entity per header, not per translation unit: its
    /// first declaration's file tells the copies apart.
    if(!decl->isExternallyVisible() && decl->getDeclContext()->getRedeclContext()->isFileContext()) {
        add_path(hasher, decl->getLocation());
    }
}

void EntityTable::add_function(Hasher& hasher, const clang::FunctionDecl* function) {
    using namespace clang;

    if(function->getPrimaryTemplate() && function->getTemplateSpecializationArgs()) {
        hasher.add(Tag::FunctionSpecialization);
        hasher.add(entity(function->getPrimaryTemplate()));
        add_template_arguments(hasher, function->getTemplateSpecializationArgs()->asArray());
        return;
    }

    auto name = function->getDeclName();
    switch(name.getNameKind()) {
        case DeclarationName::CXXConstructorName: {
            hasher.add(Tag::Constructor);
            break;
        }
        case DeclarationName::CXXDestructorName: {
            hasher.add(Tag::Destructor);
            break;
        }
        case DeclarationName::CXXConversionFunctionName: {
            hasher.add(Tag::Conversion);
            add_type(hasher, name.getCXXNameType());
            break;
        }
        case DeclarationName::CXXOperatorName: {
            hasher.add(Tag::Operator);
            hasher.add(static_cast<std::uint64_t>(name.getCXXOverloadedOperator()));
            break;
        }
        case DeclarationName::CXXLiteralOperatorName: {
            hasher.add(Tag::Literal);
            hasher.add(name.getCXXLiteralIdentifier()->getName());
            break;
        }
        case DeclarationName::CXXDeductionGuideName: {
            hasher.add(Tag::DeductionGuide);
            hasher.add(entity(name.getCXXDeductionGuideTemplate()));
            break;
        }
        default: {
            hasher.add(Tag::Function);
            hasher.add(function->getName());
            break;
        }
    }

    /// The type as written: the first declaration's spelling agrees across
    /// translation units, while getType() is rewritten to the deduced
    /// return type in whichever unit defines the function.
    auto* written = function->getTypeSourceInfo();
    add_type(hasher, written ? written->getType() : function->getType());

    if(auto* method = dyn_cast<CXXMethodDecl>(function)) {
        hasher.add(static_cast<std::uint64_t>(method->isExplicitObjectMemberFunction()));
    }

    add_expr(hasher, function->getTrailingRequiresClause().ConstraintExpr);

    if(function->isMultiVersion()) {
        if(auto* target = function->getAttr<TargetAttr>()) {
            hasher.add(target->getFeaturesStr());
        }
    }

    if(auto* described = function->getDescribedFunctionTemplate()) {
        add_template_head(hasher, described->getTemplateParameters());
    }

    if(function->isMemberLikeConstrainedFriend()) {
        hasher.add(entity(cast<CXXRecordDecl>(function->getLexicalDeclContext())));
    }
}

void EntityTable::add_location(Hasher& hasher, clang::SourceLocation location) {
    hasher.add(Tag::Location);
    if(location.isInvalid()) {
        return;
    }
    auto expansion = unit.expansion_location(location);
    auto [fid, offset] = unit.decompose_location(expansion);
    hasher.add(unit.is_builtin_file(fid) ? llvm::StringRef() : unit.file_path(fid));
    hasher.add(static_cast<std::uint64_t>(offset));
    hasher.add(static_cast<std::uint64_t>(
        unit.decompose_location(unit.spelling_location(location)).second));
}

void EntityTable::add_path(Hasher& hasher, clang::SourceLocation location) {
    hasher.add(Tag::Path);
    if(location.isInvalid()) {
        return;
    }
    auto fid = unit.decompose_location(unit.expansion_location(location)).first;
    hasher.add(unit.is_builtin_file(fid) ? llvm::StringRef() : unit.file_path(fid));
}

void EntityTable::add_declaration_name(Hasher& hasher, clang::DeclarationName name) {
    using namespace clang;

    hasher.add(Tag::Name);
    hasher.add(static_cast<std::uint64_t>(name.getNameKind()));
    switch(name.getNameKind()) {
        case DeclarationName::Identifier: {
            if(auto* identifier = name.getAsIdentifierInfo()) {
                hasher.add(identifier->getName());
            }
            break;
        }
        case DeclarationName::ObjCZeroArgSelector:
        case DeclarationName::ObjCOneArgSelector:
        case DeclarationName::ObjCMultiArgSelector: {
            hasher.add(name.getObjCSelector().getAsString());
            break;
        }
        case DeclarationName::CXXConstructorName:
        case DeclarationName::CXXDestructorName:
        case DeclarationName::CXXConversionFunctionName: {
            add_type(hasher, name.getCXXNameType());
            break;
        }
        case DeclarationName::CXXOperatorName: {
            hasher.add(static_cast<std::uint64_t>(name.getCXXOverloadedOperator()));
            break;
        }
        case DeclarationName::CXXLiteralOperatorName: {
            hasher.add(name.getCXXLiteralIdentifier()->getName());
            break;
        }
        case DeclarationName::CXXDeductionGuideName: {
            hasher.add(entity(name.getCXXDeductionGuideTemplate()));
            break;
        }
        case DeclarationName::CXXUsingDirective: {
            break;
        }
    }
}

void EntityTable::add_type(Hasher& hasher, clang::QualType type) {
    hasher.add(Tag::Type);
    hasher.add(type_hash(type));
}

void EntityTable::add_expr(Hasher& hasher, const clang::Expr* expr) {
    hasher.add(Tag::Expr);
    hasher.add(static_cast<std::uint64_t>(expr != nullptr));
    if(expr) {
        hasher.add(expr_hash(expr));
    }
}

void EntityTable::add_nested_name_specifier(Hasher& hasher,
                                            clang::NestedNameSpecifier specifier) {
    using namespace clang;

    specifier = specifier.getCanonical();
    hasher.add(Tag::NestedNameSpecifier);
    hasher.add(static_cast<std::uint64_t>(specifier.getKind()));
    switch(specifier.getKind()) {
        case NestedNameSpecifier::Kind::Null:
        case NestedNameSpecifier::Kind::Global: {
            break;
        }
        case NestedNameSpecifier::Kind::Namespace: {
            auto [ns, prefix] = specifier.getAsNamespaceAndPrefix();
            hasher.add(entity(ns));
            add_nested_name_specifier(hasher, prefix);
            break;
        }
        case NestedNameSpecifier::Kind::Type: {
            add_type(hasher, QualType(specifier.getAsType(), 0));
            break;
        }
        case NestedNameSpecifier::Kind::MicrosoftSuper: {
            hasher.add(entity(specifier.getAsMicrosoftSuper()));
            break;
        }
    }
}

void EntityTable::add_template_name(Hasher& hasher, clang::TemplateName name) {
    using namespace clang;

    hasher.add(Tag::TemplateName);
    hasher.add(static_cast<std::uint64_t>(name.getKind()));
    switch(name.getKind()) {
        case TemplateName::Template: {
            auto* decl = name.getAsTemplateDecl();
            if(auto* parameter = dyn_cast<TemplateTemplateParmDecl>(decl)) {
                hasher.add(static_cast<std::uint64_t>(parameter->getDepth()));
                hasher.add(static_cast<std::uint64_t>(parameter->getIndex()));
                hasher.add(static_cast<std::uint64_t>(parameter->isParameterPack()));
            } else {
                hasher.add(entity(decl));
            }
            break;
        }
        case TemplateName::QualifiedTemplate: {
            add_template_name(hasher, name.getAsQualifiedTemplateName()->getUnderlyingTemplate());
            break;
        }
        case TemplateName::UsingTemplate: {
            hasher.add(entity(name.getAsUsingShadowDecl()->getTargetDecl()));
            break;
        }
        case TemplateName::SubstTemplateTemplateParm: {
            add_template_name(hasher, name.getAsSubstTemplateTemplateParm()->getReplacement());
            break;
        }
        case TemplateName::SubstTemplateTemplateParmPack: {
            add_template_argument(hasher,
                                  name.getAsSubstTemplateTemplateParmPack()->getArgumentPack());
            break;
        }
        case TemplateName::DependentTemplate: {
            auto* dependent = name.getAsDependentTemplateName();
            add_nested_name_specifier(hasher, dependent->getQualifier());
            auto identifier_or_operator = dependent->getName();
            if(auto* identifier = identifier_or_operator.getIdentifier()) {
                hasher.add(identifier->getName());
            } else {
                hasher.add(static_cast<std::uint64_t>(identifier_or_operator.getOperator()));
            }
            break;
        }
        case TemplateName::OverloadedTemplate: {
            for(auto* decl: *name.getAsOverloadedTemplate()) {
                hasher.add(entity(decl));
            }
            break;
        }
        case TemplateName::AssumedTemplate: {
            add_declaration_name(hasher, name.getAsAssumedTemplateName()->getDeclName());
            break;
        }
        case TemplateName::DeducedTemplate: {
            auto* deduced = name.getAsDeducedTemplateName();
            add_template_name(hasher, deduced->getUnderlying());
            auto defaults = deduced->getDefaultArguments();
            hasher.add(static_cast<std::uint64_t>(defaults.StartPos));
            add_template_arguments(hasher, defaults.Args);
            break;
        }
    }
}

void EntityTable::add_template_argument(Hasher& hasher, const clang::TemplateArgument& argument) {
    using namespace clang;

    hasher.add(Tag::TemplateArgument);
    hasher.add(static_cast<std::uint64_t>(argument.getKind()));
    switch(argument.getKind()) {
        case TemplateArgument::Null: {
            break;
        }
        case TemplateArgument::Type: {
            add_type(hasher, argument.getAsType());
            break;
        }
        case TemplateArgument::Declaration: {
            add_type(hasher, argument.getParamTypeForDecl());
            hasher.add(entity(argument.getAsDecl()));
            break;
        }
        case TemplateArgument::NullPtr: {
            add_type(hasher, argument.getNullPtrType());
            break;
        }
        case TemplateArgument::Integral: {
            add_type(hasher, argument.getIntegralType());
            hasher.add(argument.getAsIntegral());
            break;
        }
        case TemplateArgument::StructuralValue: {
            add_type(hasher, argument.getStructuralValueType());
            add_value(hasher, argument.getAsStructuralValue());
            break;
        }
        case TemplateArgument::Template: {
            add_template_name(hasher, argument.getAsTemplate());
            break;
        }
        case TemplateArgument::TemplateExpansion: {
            add_template_name(hasher, argument.getAsTemplateOrTemplatePattern());
            hasher.add(static_cast<std::uint64_t>(argument.getNumTemplateExpansions().toInternalRepresentation()));
            break;
        }
        case TemplateArgument::Expression: {
            add_expr(hasher, argument.getAsExpr());
            break;
        }
        case TemplateArgument::Pack: {
            add_template_arguments(hasher, argument.pack_elements());
            break;
        }
    }
}

void EntityTable::add_template_arguments(Hasher& hasher,
                                         llvm::ArrayRef<clang::TemplateArgument> arguments) {
    hasher.add(static_cast<std::uint64_t>(arguments.size()));
    for(auto& argument: arguments) {
        add_template_argument(hasher, argument);
    }
}

void EntityTable::add_template_head(Hasher& hasher,
                                    const clang::TemplateParameterList* parameters) {
    using namespace clang;

    hasher.add(Tag::TemplateHead);
    hasher.add(static_cast<std::uint64_t>(parameters->size()));
    for(auto* parameter: *parameters) {
        hasher.add(static_cast<std::uint64_t>(parameter->getKind()));
        if(auto* type_parameter = dyn_cast<TemplateTypeParmDecl>(parameter)) {
            hasher.add(static_cast<std::uint64_t>(type_parameter->isParameterPack()));
            auto* constraint = type_parameter->getTypeConstraint();
            add_expr(hasher, constraint ? constraint->getImmediatelyDeclaredConstraint() : nullptr);
        } else if(auto* value_parameter = dyn_cast<NonTypeTemplateParmDecl>(parameter)) {
            hasher.add(static_cast<std::uint64_t>(value_parameter->isParameterPack()));
            add_type(hasher, value_parameter->getType());
            add_expr(hasher, value_parameter->getPlaceholderTypeConstraint());
        } else {
            auto* template_parameter = cast<TemplateTemplateParmDecl>(parameter);
            hasher.add(static_cast<std::uint64_t>(template_parameter->isParameterPack()));
            add_template_head(hasher, template_parameter->getTemplateParameters());
        }
    }
    add_expr(hasher, parameters->getRequiresClause());
}

void EntityTable::add_value(Hasher& hasher, const clang::APValue& value) {
    using namespace clang;

    hasher.add(Tag::Value);
    hasher.add(static_cast<std::uint64_t>(value.getKind()));
    switch(value.getKind()) {
        case APValue::None:
        case APValue::Indeterminate: {
            break;
        }
        case APValue::Int: {
            hasher.add(value.getInt());
            break;
        }
        case APValue::Float: {
            hasher.add(value.getFloat());
            break;
        }
        case APValue::FixedPoint: {
            auto fixed = value.getFixedPoint();
            hasher.add(fixed.getValue());
            hasher.add(static_cast<std::uint64_t>(fixed.getScale()));
            break;
        }
        case APValue::ComplexInt: {
            hasher.add(value.getComplexIntReal());
            hasher.add(value.getComplexIntImag());
            break;
        }
        case APValue::ComplexFloat: {
            hasher.add(value.getComplexFloatReal());
            hasher.add(value.getComplexFloatImag());
            break;
        }
        case APValue::LValue: {
            auto base = value.getLValueBase();
            if(base.isNull()) {
                hasher.add(Tag::Null);
            } else if(auto* decl = base.dyn_cast<const ValueDecl*>()) {
                hasher.add(entity(decl));
            } else if(auto* expr = base.dyn_cast<const Expr*>()) {
                add_expr(hasher, expr);
            } else if(base.is<TypeInfoLValue>()) {
                add_type(hasher, base.getTypeInfoType());
            } else {
                hasher.add(static_cast<std::uint64_t>(base.get<DynamicAllocLValue>().getIndex()));
                add_type(hasher, base.getDynamicAllocType());
            }
            hasher.add(static_cast<std::uint64_t>(value.getLValueOffset().getQuantity()));
            hasher.add(static_cast<std::uint64_t>(value.isNullPointer()));
            hasher.add(static_cast<std::uint64_t>(value.isLValueOnePastTheEnd()));
            hasher.add(static_cast<std::uint64_t>(value.hasLValuePath()));
            if(!value.hasLValuePath()) {
                break;
            }
            /// The path alternates between array indices and subobject
            /// declarations; only the type being walked tells which.
            QualType so_far = base.getType();
            for(auto entry: value.getLValuePath()) {
                if(auto* array = so_far->getAsArrayTypeUnsafe()) {
                    hasher.add(static_cast<std::uint64_t>(entry.getAsArrayIndex()));
                    so_far = array->getElementType();
                    continue;
                }
                auto member = entry.getAsBaseOrMember();
                auto* named = cast<NamedDecl>(member.getPointer());
                hasher.add(entity(named));
                hasher.add(static_cast<std::uint64_t>(member.getInt()));
                if(auto* field = dyn_cast<FieldDecl>(named)) {
                    so_far = field->getType();
                } else {
                    so_far = unit.context().getCanonicalTagType(cast<CXXRecordDecl>(named));
                }
            }
            break;
        }
        case APValue::Vector: {
            hasher.add(static_cast<std::uint64_t>(value.getVectorLength()));
            for(unsigned i = 0; i < value.getVectorLength(); i += 1) {
                add_value(hasher, value.getVectorElt(i));
            }
            break;
        }
        case APValue::Array: {
            hasher.add(static_cast<std::uint64_t>(value.getArraySize()));
            hasher.add(static_cast<std::uint64_t>(value.getArrayInitializedElts()));
            for(unsigned i = 0; i < value.getArrayInitializedElts(); i += 1) {
                add_value(hasher, value.getArrayInitializedElt(i));
            }
            if(value.hasArrayFiller()) {
                add_value(hasher, value.getArrayFiller());
            }
            break;
        }
        case APValue::Struct: {
            hasher.add(static_cast<std::uint64_t>(value.getStructNumBases()));
            for(unsigned i = 0; i < value.getStructNumBases(); i += 1) {
                add_value(hasher, value.getStructBase(i));
            }
            hasher.add(static_cast<std::uint64_t>(value.getStructNumFields()));
            for(unsigned i = 0; i < value.getStructNumFields(); i += 1) {
                add_value(hasher, value.getStructField(i));
            }
            break;
        }
        case APValue::Union: {
            if(auto* field = value.getUnionField()) {
                hasher.add(entity(field));
                add_value(hasher, value.getUnionValue());
            }
            break;
        }
        case APValue::MemberPointer: {
            if(auto* member = value.getMemberPointerDecl()) {
                hasher.add(entity(member));
            }
            hasher.add(static_cast<std::uint64_t>(value.isMemberPointerToDerivedMember()));
            for(auto* record: value.getMemberPointerPath()) {
                hasher.add(entity(record));
            }
            break;
        }
        case APValue::AddrLabelDiff: {
            add_expr(hasher, value.getAddrLabelDiffLHS());
            add_expr(hasher, value.getAddrLabelDiffRHS());
            break;
        }
    }
}

}  // namespace clice
