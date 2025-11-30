#include "Test/Test.h"
#include "Compiler/Compilation.h"
#include "Compiler/Diagnostic.h"

namespace clice::testing {

// see llvm/clang/include/clang/AST/ASTDiagnostic.h
void dump_arg(clang::DiagnosticsEngine::ArgumentKind kind, std::uint64_t value) {
    switch(kind) {
        case clang::DiagnosticsEngine::ak_identifierinfo: {
            clang::IdentifierInfo* info = reinterpret_cast<clang::IdentifierInfo*>(value);
            llvm::outs() << info->getName();
            break;
        }

        case clang::DiagnosticsEngine::ak_qual: {
            clang::Qualifiers qual = clang::Qualifiers::fromOpaqueValue(value);
            llvm::outs() << qual.getAsString();
            break;
        }

        case clang::DiagnosticsEngine::ak_qualtype: {
            clang::QualType type =
                clang::QualType::getFromOpaquePtr(reinterpret_cast<void*>(value));
            llvm::outs() << type.getAsString();
            break;
        }

        case clang::DiagnosticsEngine::ak_qualtype_pair: {
            clang::TemplateDiffTypes& TDT = *reinterpret_cast<clang::TemplateDiffTypes*>(value);
            clang::QualType type1 =
                clang::QualType::getFromOpaquePtr(reinterpret_cast<void*>(TDT.FromType));
            clang::QualType type2 =
                clang::QualType::getFromOpaquePtr(reinterpret_cast<void*>(TDT.ToType));
            llvm::outs() << type1.getAsString() << " -> " << type2.getAsString();
            break;
        }

        case clang::DiagnosticsEngine::ak_declarationname: {
            clang::DeclarationName name = clang::DeclarationName::getFromOpaqueInteger(value);
            llvm::outs() << name.getAsString();
            break;
        }

        case clang::DiagnosticsEngine::ak_nameddecl: {
            clang::NamedDecl* decl = reinterpret_cast<clang::NamedDecl*>(value);
            llvm::outs() << decl->getNameAsString();
            break;
        }

        case clang::DiagnosticsEngine::ak_nestednamespec: {
            clang::NestedNameSpecifier* spec = reinterpret_cast<clang::NestedNameSpecifier*>(value);
            spec->dump();
            break;
        }

        case clang::DiagnosticsEngine::ak_declcontext: {
            clang::DeclContext* context = reinterpret_cast<clang::DeclContext*>(value);
            llvm::outs() << context->getDeclKindName();
            break;
        }

        case clang::DiagnosticsEngine::ak_attr: {
            clang::Attr* attr = reinterpret_cast<clang::Attr*>(value);
            break;
            // attr->dump();
        }

        default: {
            std::abort();
        }
    }

    llvm::outs() << "\n";
}

namespace {

using namespace clice;

TEST_SUITE(Diagnostic) {

TEST_CASE(CommandError) {
    CompilationParams params;
    /// miss input file.
    params.arguments = {"clang++"};
    params.add_remapped_file("main.cpp", "int main() { return 0; }");
    auto unit = compile(params);
    ASSERT_FALSE(unit.has_value());
}

TEST_CASE(Error) {
    CompilationParams params;
    params.arguments = {"clang++", "main.cpp"};
    params.add_remapped_file("main.cpp", "int main() { return 0 }");
    auto unit = compile(params);
    ASSERT_TRUE(unit.has_value());
    ASSERT_FALSE(unit->diagnostics().empty());

    /// for(auto& diag: unit->diagnostics()) {
    ///     std::println("{}", diag.message);
    /// }
};

TEST_CASE(PCHError) {
    /// Any error in compilation will result in failure on generating PCH or PCM.
    CompilationParams params;
    params.arguments = {"clang++", "main.cpp"};
    params.output_file = "fake.pch";
    params.add_remapped_file("main.cpp", R"(
void foo() {}
void foo() {}
)");

    PCHInfo info;
    auto unit = compile(params, info);
    ASSERT_FALSE(unit.has_value());
}

TEST_CASE(ASTError) {
    /// Event fatal error may generate incomplete AST, but it is fine.
    CompilationParams params;
    params.arguments = {"clang++", "main.cpp"};
    params.add_remapped_file("main.cpp", R"(
void foo() {}
void foo() {}
)");

    PCHInfo info;
    auto unit = compile(params);
    ASSERT_TRUE(unit.has_value());
}

};  // TEST_SUITE(Diagnostic)

}  // namespace

}  // namespace clice::testing
