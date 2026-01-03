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

TEST_CASE(TargetError) {
    CompilationParams params;
    params.arguments = {"clang++", "-target", "aa-bb-cc", "main.cpp"};
    params.add_remapped_file("main.cpp", "");

    auto unit = compile(params);
    ASSERT_TRUE(unit.setup_fail());
    ASSERT_TRUE(unit.diagnostics().size() == 1);

    auto& diag = unit.diagnostics()[0];
    EXPECT_EQ(diag.id.diagnostic_code(), "err_target_unknown_triple");
    EXPECT_EQ(diag.id.level, DiagnosticLevel::Error);
    EXPECT_EQ(diag.id.source, DiagnosticSource::Clang);
    EXPECT_TRUE(diag.fid.isInvalid());
    EXPECT_TRUE(!diag.range.valid());
    EXPECT_EQ(diag.message, "unknown target triple 'aa-bb-cc'");
}

TEST_CASE(Error) {
    CompilationParams params;
    params.arguments = {"clang++", "main.cpp"};
    params.add_remapped_file("main.cpp", "int main() { return 0 }");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_TRUE(unit.diagnostics().size() == 1);

    auto& diag = unit.diagnostics()[0];
    EXPECT_EQ(diag.id.diagnostic_code(), "err_expected_semi_after_stmt");
    EXPECT_EQ(diag.id.level, DiagnosticLevel::Error);
    EXPECT_EQ(diag.id.source, DiagnosticSource::Clang);
    EXPECT_EQ(diag.fid, unit.interested_file());
    EXPECT_TRUE(diag.range.valid());
    EXPECT_EQ(diag.message, "expected ';' after return statement");
};

TEST_CASE(Warning) {
    CompilationParams params;
    params.arguments = {"clang++", "-Wall", "-Wunused-variable", "main.cpp"};
    params.add_remapped_file("main.cpp", "int main() { int x; return 0; }");

    auto unit = compile(params);
    ASSERT_TRUE(unit.completed());
    ASSERT_EQ(unit.diagnostics().size(), 1);

    auto& diag = unit.diagnostics()[0];
    EXPECT_EQ(diag.id.diagnostic_code(), "warn_unused_variable");
    EXPECT_EQ(diag.id.level, DiagnosticLevel::Warning);
    EXPECT_EQ(diag.id.source, DiagnosticSource::Clang);
    EXPECT_TRUE(diag.range.valid());
    EXPECT_TRUE(diag.message.find("unused variable") != std::string::npos);
}

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
    ASSERT_TRUE(unit.fatal_error());
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
    ASSERT_TRUE(unit.completed());
}

};  // TEST_SUITE(Diagnostic)

}  // namespace

}  // namespace clice::testing
