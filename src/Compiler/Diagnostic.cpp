#include "Compiler/Diagnostic.h"
#include "clang/AST/Type.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/AllDiagnostics.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"
#include "Support/Format.h"

namespace clice {

llvm::StringRef DiagnosticID::diagnostic_code() const {
    switch(value) {
#define DIAG(ENUM,                                                                                 \
             CLASS,                                                                                \
             DEFAULT_MAPPING,                                                                      \
             DESC,                                                                                 \
             GROPU,                                                                                \
             SFINAE,                                                                               \
             NOWERROR,                                                                             \
             SHOWINSYSHEADER,                                                                      \
             SHOWINSYSMACRO,                                                                       \
             DEFERRABLE,                                                                           \
             CATEGORY)                                                                             \
    case clang::diag::ENUM: return #ENUM;
#include "clang/Basic/DiagnosticASTKinds.inc"
#include "clang/Basic/DiagnosticAnalysisKinds.inc"
#include "clang/Basic/DiagnosticCommentKinds.inc"
#include "clang/Basic/DiagnosticCommonKinds.inc"
#include "clang/Basic/DiagnosticDriverKinds.inc"
#include "clang/Basic/DiagnosticFrontendKinds.inc"
#include "clang/Basic/DiagnosticLexKinds.inc"
#include "clang/Basic/DiagnosticParseKinds.inc"
#include "clang/Basic/DiagnosticRefactoringKinds.inc"
#include "clang/Basic/DiagnosticSemaKinds.inc"
#include "clang/Basic/DiagnosticSerializationKinds.inc"
#undef DIAG
        default: return llvm::StringRef();
    }
}

std::optional<std::string> DiagnosticID::diagnostic_document_uri() const {
    switch(source) {
        case DiagnosticSource::Unknown:
        case DiagnosticSource::Clang: {
            // There is a page listing many warning flags, but it provides too little
            // information to be worth linking.
            // https://clang.llvm.org/docs/DiagnosticsReference.html
            return std::nullopt;
        }

        case DiagnosticSource::ClangTidy: {
            // This won't correctly get the module for clang-analyzer checks, but as we
            // don't link in the analyzer that shouldn't be an issue.
            // This would also need updating if anyone decides to create a module with a
            // '-' in the name.
            auto [module, check] = name.split('-');
            if(module.empty() || check.empty()) {
                return std::nullopt;
            }

            return std::format("https://clang.llvm.org/extra/clang-tidy/checks/{}/{}.html",
                               module,
                               check);
        }

        case DiagnosticSource::Clice: {
            /// TODO: Add diagnostic for clice.
            return std::nullopt;
        }
    }
}

bool DiagnosticID::is_deprecated() const {
    namespace diag = clang::diag;
    static llvm::DenseSet<std::uint32_t> deprecated_diags{
        diag::warn_access_decl_deprecated,
        diag::warn_atl_uuid_deprecated,
        diag::warn_deprecated,
        diag::warn_deprecated_altivec_src_compat,
        diag::warn_deprecated_comma_subscript,
        diag::warn_deprecated_copy,
        diag::warn_deprecated_copy_with_dtor,
        diag::warn_deprecated_copy_with_user_provided_copy,
        diag::warn_deprecated_copy_with_user_provided_dtor,
        diag::warn_deprecated_def,
        diag::warn_deprecated_increment_decrement_volatile,
        diag::warn_deprecated_message,
        diag::warn_deprecated_redundant_constexpr_static_def,
        diag::warn_deprecated_register,
        diag::warn_deprecated_simple_assign_volatile,
        diag::warn_deprecated_string_literal_conversion,
        diag::warn_deprecated_this_capture,
        diag::warn_deprecated_volatile_param,
        diag::warn_deprecated_volatile_return,
        diag::warn_deprecated_volatile_structured_binding,
        diag::warn_opencl_attr_deprecated_ignored,
        diag::warn_property_method_deprecated,
        diag::warn_vector_mode_deprecated,
    };

    /// TODO: Add clang tidy
    return source == DiagnosticSource::Clang && deprecated_diags.contains(value);
}

bool DiagnosticID::is_unused() const {
    namespace diag = clang::diag;
    static llvm::DenseSet<std::uint32_t> unused_diags = {
        diag::warn_opencl_attr_deprecated_ignored,
        diag::warn_pragma_attribute_unused,
        diag::warn_unused_but_set_parameter,
        diag::warn_unused_but_set_variable,
        diag::warn_unused_comparison,
        diag::warn_unused_const_variable,
        diag::warn_unused_exception_param,
        diag::warn_unused_function,
        diag::warn_unused_label,
        diag::warn_unused_lambda_capture,
        diag::warn_unused_local_typedef,
        diag::warn_unused_member_function,
        diag::warn_unused_parameter,
        diag::warn_unused_private_field,
        diag::warn_unused_property_backing_ivar,
        diag::warn_unused_template,
        diag::warn_unused_variable,
    };

    /// TODO: Add clang tidy
    return source == DiagnosticSource::Clang && unused_diags.contains(value);
}

bool is_note(clang::DiagnosticsEngine::Level level) {
    return level == clang::DiagnosticsEngine::Note || level == clang::DiagnosticsEngine::Remark;
}

static DiagnosticLevel diagnostic_level(clang::DiagnosticsEngine::Level level) {
    switch(level) {
        case clang::DiagnosticsEngine::Ignored: return DiagnosticLevel::Ignored;
        case clang::DiagnosticsEngine::Note: return DiagnosticLevel::Note;
        case clang::DiagnosticsEngine::Remark: return DiagnosticLevel::Remark;
        case clang::DiagnosticsEngine::Warning: return DiagnosticLevel::Warning;
        case clang::DiagnosticsEngine::Error: return DiagnosticLevel::Error;
        case clang::DiagnosticsEngine::Fatal: return DiagnosticLevel::Fatal;
        default: return DiagnosticLevel::Invalid;
    }
}

/// Get the range for given diagnostic.
/// FIXME: I would like to use `CompilationUnit`.
auto diagnostic_range(const clang::Diagnostic& diagnostic, const clang::LangOptions& options)
    -> std::optional<std::pair<clang::FileID, LocalSourceRange>> {
    /// If location is invalid, it represents the diagnostic is
    /// from the command line.
    auto location = diagnostic.getLocation();
    if(location.isInvalid()) {
        return std::nullopt;
    }

    /// If the location is valid, the `SourceManager` is valid too.
    auto& src_mgr = diagnostic.getDiags()->getSourceManager();

    /// Make sure the location is file location.
    location = src_mgr.getFileLoc(location);
    assert(location.isFileID());

    auto [fid, offset] = src_mgr.getDecomposedLoc(location);

    /// Select a proper range for the diagnostic.
    for(auto range: diagnostic.getRanges()) {
        range = clang::Lexer::makeFileCharRange(range, src_mgr, options);

        auto [begin, end] = range.getAsRange();
        auto [begin_fid, begin_offset] = src_mgr.getDecomposedLoc(begin);
        if(begin_fid != fid || begin_offset <= offset) {
            continue;
        }

        auto [end_fid, end_offset] = src_mgr.getDecomposedLoc(end);
        if(range.isTokenRange()) {
            end_offset += clang::Lexer::MeasureTokenLength(end, src_mgr, options);
        }

        if(end_fid == fid && end_offset >= offset) {
            return std::pair{
                fid,
                LocalSourceRange{begin_offset, end_offset}
            };
        }
    }

    /// Use token range.
    auto end_offset = offset + clang::Lexer::MeasureTokenLength(location, src_mgr, options);
    return std::pair{
        fid,
        LocalSourceRange{offset, end_offset}
    };
}

class DiagnosticCollectorImpl : public clang::DiagnosticConsumer, public DiagnosticCollector {
public:
    DiagnosticCollectorImpl(std::shared_ptr<std::vector<Diagnostic>> diagnostics) :
        diagnostics(diagnostics) {}

    void BeginSourceFile(const clang::LangOptions& Opts, const clang::Preprocessor* PP) override {
        options = &Opts;
        src_mgr = &PP->getSourceManager();
    }

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& raw_diagnostic) override {

        auto& diagnostic = diagnostics->emplace_back();
        diagnostic.id.value = raw_diagnostic.getID();

        if(!is_note(level)) {
            if(transform)
                level = transform->adjust_level(level, raw_diagnostic);
        }
        diagnostic.id.level = diagnostic_level(level);

        /// TODO:
        // use DiagnosticEngine::SetArgToStringFn to set a custom function to convert arguments to
        // strings. Support markdown diagnostic in LSP 3.18. allow complex type to display in
        // markdown code block.
        ///
        /// auto& engine = src_mgr->getDiagnostics();
        /// engine.SetArgToStringFn();

        llvm::SmallString<256> message;
        raw_diagnostic.FormatDiagnostic(message);
        diagnostic.message = message.str();

        if(auto pair = diagnostic_range(raw_diagnostic, *options)) {
            auto [fid, range] = *pair;
            diagnostic.fid = fid;
            diagnostic.range = range;
        }

        if(transform) {
            transform->adjust_diag(diagnostic);
        }

        /// TODO: handle FixIts
        /// raw_diagnostic.getFixItHints();
    }

    void EndSourceFile() override {}

    void set_transform(DiagnosticTransform* transform) override {
        this->transform = transform;
    }

private:
    std::shared_ptr<std::vector<Diagnostic>> diagnostics;
    const clang::LangOptions* options;
    clang::SourceManager* src_mgr;
    DiagnosticTransform* transform = nullptr;
};

std::pair<DiagnosticCollector*, clang::DiagnosticConsumer*>
    Diagnostic::create(std::shared_ptr<std::vector<Diagnostic>> diagnostics) {
    auto collector = new DiagnosticCollectorImpl(diagnostics);
    return std::pair{collector, collector};
}

}  // namespace clice
