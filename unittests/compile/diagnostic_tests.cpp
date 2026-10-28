module;

#include "support/prelude.h"

module clice;

import :compile.compilation;
import :compile.diagnostic;
import :feature.diagnostics;
import :feature.protocol;
import :syntax.position;
import :unittests.utils;

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

ZEST_SUITE(Diagnostic) {

/// Holds VFS-backed CompilationParams.
struct DiagParams {
    llvm::IntrusiveRefCntPtr<TestVFS> vfs;
    CompilationParams params;

    DiagParams(llvm::StringRef content, std::initializer_list<const char*> extra_args = {}) {
        vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
        vfs->add("main.cpp", content);
        params.kind = CompilationKind::Content;
        params.input.vfs = vfs;

        params.input.arguments.push_back("clang++");
        params.input.arguments.push_back("-ffreestanding");
        params.input.arguments.push_back("-Xclang");
        params.input.arguments.push_back("-undef");
        for(auto a: extra_args) {
            params.input.arguments.push_back(a);
        }
        params.input.arguments.push_back(TestVFS::path("main.cpp"));
    }
};

ZEST_CASE(TargetError) {
    auto vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    vfs->add("main.cpp", "");

    std::string main_path = TestVFS::path("main.cpp");
    CompilationParams params;
    params.kind = CompilationKind::Content;
    params.input.vfs = vfs;
    params.input.arguments = {"clang++", "-target", "aa-bb-cc", main_path};

    auto unit = compile(std::move(params));
    ZASSERT(unit->setup_fail());
    ZASSERT(unit->diagnostics().size() == 1);

    auto& diag = unit->diagnostics()[0];
    ZEXPECT(diag.id.diagnostic_code() == "err_target_unknown_triple");
    ZEXPECT(diag.id.level == DiagnosticLevel::Error);
    ZEXPECT(diag.id.source == DiagnosticSource::Clang);
    ZEXPECT(diag.fid.isInvalid());
    ZEXPECT(!diag.range.valid());
    ZEXPECT(diag.message == "unknown target triple 'aa-bb-cc'");
}

ZEST_CASE(Error) {
    DiagParams dp("int main() { return 0 }");

    auto unit = compile(std::move(dp.params));
    ZASSERT(unit->completed());
    ZASSERT(unit->diagnostics().size() == 1);

    auto& diag = unit->diagnostics()[0];
    ZEXPECT(diag.id.diagnostic_code() == "err_expected_semi_after_stmt");
    ZEXPECT(diag.id.level == DiagnosticLevel::Error);
    ZEXPECT(diag.id.source == DiagnosticSource::Clang);
    ZEXPECT(diag.fid == unit->focus_file().file());
    ZEXPECT(diag.range.valid());
    ZEXPECT(diag.message == "expected ';' after return statement");
};

ZEST_CASE(Warning) {
    DiagParams dp("int main() { int x; return 0; }", {"-Wall", "-Wunused-variable"});

    auto unit = compile(std::move(dp.params));
    ZASSERT(unit->completed());
    ZASSERT(unit->diagnostics().size() == 1);

    auto& diag = unit->diagnostics()[0];
    ZEXPECT(diag.id.diagnostic_code() == "-Wunused-variable");
    ZEXPECT(diag.id.level == DiagnosticLevel::Warning);
    ZEXPECT(diag.id.source == DiagnosticSource::Clang);
    ZEXPECT(diag.range.valid());
    ZEXPECT(diag.message.find("unused variable") != std::string::npos);
}

ZEST_CASE(NotesAttached) {
    DiagParams dp("void foo() {}\nvoid foo() {}\n");

    auto unit = compile(std::move(dp.params));
    ZASSERT(unit->completed());
    ZASSERT(unit->diagnostics().size() == 1);

    auto& diag = unit->diagnostics()[0];
    ZEXPECT(diag.id.diagnostic_code() == "err_redefinition");
    ZASSERT(diag.notes.size() == 1);
    ZEXPECT(diag.notes[0].id.level == DiagnosticLevel::Note);
    ZEXPECT(diag.notes[0].message == "previous definition is here");
    ZEXPECT(diag.notes[0].range.begin == 5);
}

ZEST_CASE(PCMError) {
    /// A PCM build with errors writes no PCM.
    DiagParams dp(R"(
void foo() {}
void foo() {}
)");
    dp.params.kind = CompilationKind::ModuleInterface;
    dp.params.output_file = "fake.pcm";

    auto unit = compile(std::move(dp.params));
    ZASSERT(unit->completed());
    ZEXPECT(!unit->has_output());
}

ZEST_CASE(ASTError) {
    /// Event fatal error may generate incomplete AST, but it is fine.
    DiagParams dp(R"(
void foo() {}
void foo() {}
)");

    auto unit = compile(std::move(dp.params));
    ZASSERT(unit->completed());
}

ZEST_CASE(CommandLineNote) {
    /// A macro-redefinition note points into <command line>, no file a
    /// note can name: it is left out.
    DiagParams dp(R"(
#define FOO 2
int main() { return 0; }
)",
                  {"-DFOO=1"});

    auto unit = compile(std::move(dp.params));
    ZASSERT(unit->completed());

    auto diagnostics = feature::diagnostics(*unit);
    bool redefined = false;
    for(auto& diag: diagnostics) {
        if(diag.message.find("macro redefined") != std::string::npos) {
            redefined = true;
            ZEXPECT(diag.notes.empty());
        }
    }
    ZASSERT(redefined);
}

ZEST_CASE(NotesPlacedWhenShown) {
    /// A note in the diagnostic's own file is placed in the text compiled;
    /// another file is read when the diagnostic is shown: one gone, or
    /// shorter than the note's range, places nothing.
    std::string text = "int x;\n";
    auto starts = lsp::line_starts(text);
    PositionMap positions(text, starts);
    std::string header = "int \xe7\x94\xb2\xf0\x9f\x98\x80;\n";
    auto header_starts = lsp::line_starts(header);
    feature::FileDiagnostic diagnostic{
        .range = {4, 5},
        .message = "redefinition"
    };
    diagnostic.notes = {
        {.path = "",        .range = {4, 5},   .message = "own" },
        {.path = "/gone.h", .range = {0, 3},   .message = "gone"},
        {.path = "/wide.h", .range = {20, 21}, .message = "past"},
        {.path = "/wide.h", .range = {4, 11},  .message = "here"},
    };
    auto note_positions = [&](llvm::StringRef path) -> std::optional<PositionMap> {
        if(path == "/gone.h") {
            return std::nullopt;
        }
        return PositionMap(header, header_starts);
    };

    for(auto [encoding, end]: {
            std::pair{PositionEncoding::UTF8,  11u},
            std::pair{PositionEncoding::UTF16, 7u },
            std::pair{PositionEncoding::UTF32, 6u },
    }) {
        auto shown = feature::to_protocol(diagnostic,
                                          true,
                                          encoding,
                                          "/main.cpp",
                                          positions,
                                          note_positions);
        ZEXPECT(shown.range.end.character == 5u);
        ZASSERT(shown.related_information);
        auto& related = *shown.related_information;
        ZASSERT(related.size() == 2u);
        ZEXPECT(related[0].message == "own");
        ZEXPECT(related[0].location.uri == feature::to_uri("/main.cpp"));
        ZEXPECT(related[0].location.range.end.character == 5u);
        ZEXPECT(related[1].message == "here");
        ZEXPECT(related[1].location.range.end.character == end);
    }
}

};  // ZEST_SUITE(Diagnostic)

}  // namespace

}  // namespace clice::testing
