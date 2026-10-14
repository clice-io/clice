module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :support.format;

namespace clice {

llvm::StringRef diagnostic_source_name(DiagnosticSource source) {
    switch(source) {
        case DiagnosticSource::Unknown: return {};
        case DiagnosticSource::Clang: return "clang";
        case DiagnosticSource::ClangTidy: return "clang-tidy";
        case DiagnosticSource::Clice: return "clice";
    }
    std::unreachable();
}

llvm::StringRef DiagnosticID::diagnostic_code() const {
    if(source == DiagnosticSource::ClangTidy) {
        return name;
    }
    switch(value) {
#define DIAG(ENUM,                                                                                 \
             CLASS,                                                                                \
             DEFAULT_MAPPING,                                                                      \
             DESC,                                                                                 \
             GROUP,                                                                                \
             SFINAE,                                                                               \
             NOWERROR,                                                                             \
             SHOWINSYSHEADER,                                                                      \
             SHOWINSYSMACRO,                                                                       \
             DEFERRABLE,                                                                           \
             CATEGORY,                                                                             \
             STABLE_ID,                                                                            \
             LEGACY_STABLE_IDS)                                                                    \
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
            // clice's own guidance diagnostics link to the setup guide that
            // explains how to provide a compilation database.
            if(name == "inferred-compile-command") {
                return "https://docs.clice.io/clice/guide/quick-start#project-setup";
            }
            return std::nullopt;
        }
    }

    return std::nullopt;
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

    if(source == DiagnosticSource::ClangTidy) {
        return name.starts_with("modernize-");
    }
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

    if(source == DiagnosticSource::ClangTidy) {
        return name.starts_with("misc-unused-");
    }
    return source == DiagnosticSource::Clang && unused_diags.contains(value);
}

bool DiagnosticID::is_deserialization_error() const {
    // Category membership rather than an ID list: the family is large
    // (DiagnosticSerializationKinds) and grows across clang versions,
    // while the category is stable. Resolved through a member of the
    // family instead of a hard-coded category number.
    const static unsigned category =
        clang::DiagnosticIDs::getCategoryNumberForDiag(clang::diag::err_fe_ast_file_malformed);
    return source == DiagnosticSource::Clang &&
           (level == DiagnosticLevel::Error || level == DiagnosticLevel::Fatal) &&
           clang::DiagnosticIDs::getCategoryNumberForDiag(value) == category;
}

namespace {

bool is_note(clang::DiagnosticsEngine::Level level) {
    return level == clang::DiagnosticsEngine::Note || level == clang::DiagnosticsEngine::Remark;
}

DiagnosticLevel diagnostic_level(clang::DiagnosticsEngine::Level level) {
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

}  // namespace

class CompilationUnit::DiagnosticCollector : public clang::DiagnosticConsumer {
public:
    DiagnosticCollector(CompilationUnit& unit) : unit(unit) {}

    /// Where a file location is; the preamble a PCH recorded of the focus
    /// file is the focus file.
    auto decompose(clang::SourceLocation location) -> std::pair<clang::FileID, std::uint32_t> {
        auto [fid, offset] = unit.seen_at(location);
        auto focus = unit.focus_file();
        return {focus.contains(fid) ? focus.file() : fid, offset};
    }

    /// The range in `fid` a source range covers, half-open; nullopt when it
    /// crosses files or a macro expansion hides it.
    auto file_range(clang::CharSourceRange range, clang::FileID fid)
        -> std::optional<LocalSourceRange> {
        range = clang::Lexer::makeFileCharRange(range, unit.SM(), unit.lang_options());
        if(range.isInvalid()) {
            return std::nullopt;
        }
        auto [begin_fid, begin] = decompose(range.getBegin());
        auto [end_fid, end] = decompose(range.getEnd());
        if(begin_fid != fid || end_fid != fid) {
            return std::nullopt;
        }
        return LocalSourceRange{begin, end};
    }

    /// The first range that holds the caret, as clang underlines it; else
    /// the token at the caret.
    auto diagnostic_range(const clang::Diagnostic& diagnostic)
        -> std::optional<std::pair<clang::FileID, LocalSourceRange>> {
        /// If location is invalid, it represents the diagnostic is
        /// from the command line.
        auto location = diagnostic.getLocation();
        if(location.isInvalid()) {
            return std::nullopt;
        }

        /// Make sure the location is file location.
        location = unit.SM().getFileLoc(location);

        auto [fid, offset] = decompose(location);
        for(auto range: diagnostic.getRanges()) {
            if(auto local = file_range(range, fid); local && local->contains(offset)) {
                return std::pair{fid, *local};
            }
        }
        for(auto& hint: diagnostic.getFixItHints()) {
            if(auto local = file_range(hint.RemoveRange, fid); local && local->contains(offset)) {
                return std::pair{fid, *local};
            }
        }

        auto end_offset = offset + unit.token_length(location);
        return std::pair{
            fid,
            LocalSourceRange{offset, end_offset}
        };
    }

    /// The replacements a diagnostic's fix-it hints spell, all of them or
    /// none: one inside a macro expansion, but for a replacement within a
    /// single macro argument, which edits the argument, or in another file
    /// leaves the fix unexpressed, and so do edits that overlap, which an
    /// editor refuses (misc-unused-parameters removes both `f(1)` and its
    /// `1` from `f(f(1))`). An insertion is no such exception: clang drops
    /// a hint whose place it cannot find in a macro, the end of a token
    /// there, without a trace, and the insertions left in the argument may
    /// be half a fix (the `(` of a pair of parentheses).
    auto fix(const clang::Diagnostic& diagnostic) -> std::vector<TextReplacement> {
        auto& sm = unit.SM();
        std::vector<TextReplacement> edits;
        for(auto hint: diagnostic.getFixItHints()) {
            auto& range = hint.RemoveRange;
            auto begin = range.getBegin();
            auto end = range.getEnd();
            bool insertion = range.isCharRange() && begin == end;
            if(!insertion && begin.isMacroID() && end.isMacroID() &&
               sm.getFileID(begin) == sm.getFileID(end)) {
                range = clang::CharSourceRange(
                    {sm.getTopMacroCallerLoc(begin), sm.getTopMacroCallerLoc(end)},
                    range.isTokenRange());
            }
            if(range.getBegin().isMacroID() || range.getEnd().isMacroID()) {
                return {};
            }
            auto focus = unit.focus_file();
            auto removed = file_range(range, focus.file());
            if(!removed) {
                return {};
            }
            std::string text = std::move(hint.CodeToInsert);
            if(hint.InsertFromRange.isValid()) {
                auto copied = file_range(hint.InsertFromRange, focus.file());
                if(!copied) {
                    return {};
                }
                text = focus.content().substr(copied->begin, copied->length());
            }
            // An editor applies insertions at one offset in their order.
            auto at = edits.end();
            if(hint.BeforePreviousInsertions) {
                at = llvm::find_if(edits, [&](const TextReplacement& edit) {
                    return edit.range.begin == removed->begin && edit.range.length() == 0;
                });
            }
            edits.insert(at, {*removed, std::move(text)});
        }
        llvm::SmallVector<LocalSourceRange> ranges;
        for(auto& edit: edits) {
            ranges.push_back(edit.range);
        }
        llvm::sort(ranges, [](LocalSourceRange a, LocalSourceRange b) {
            return std::pair(a.begin, a.end) < std::pair(b.begin, b.end);
        });
        for(auto [before, after]: llvm::zip(ranges, llvm::drop_begin(ranges))) {
            if(after.begin < before.end) {
                return {};
            }
        }
        return edits;
    }

    void BeginSourceFile(const clang::LangOptions&, const clang::Preprocessor*) override {}

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& raw_diagnostic) override {
        // The engine only marks a fatal error once another diagnostic
        // follows it.
        if(level == clang::DiagnosticsEngine::Fatal) {
            unit.fatal = true;
        }

        // A note belongs to the diagnostic before it; one ahead of every
        // diagnostic, or after one left out, belongs to none.
        if(is_note(level)) {
            if(attach_notes) {
                auto note = convert(level, raw_diagnostic);
                unit.collected_diagnostics.back().notes.push_back(std::move(note));
            }
            return;
        }

        if(unit.checker) {
            level = unit.checker->adjust_level(level, raw_diagnostic);
        }
        auto diagnostic = convert(level, raw_diagnostic);
        if(unit.checker) {
            unit.checker->adjust_diag(diagnostic);
        }
        // A check can report one finding more than once, from several
        // instantiations of a template.
        attach_notes = diagnostic.id.level != DiagnosticLevel::Ignored &&
                       (diagnostic.id.source != DiagnosticSource::ClangTidy ||
                        findings
                            .emplace(diagnostic.fid,
                                     diagnostic.range.begin,
                                     diagnostic.range.end,
                                     diagnostic.message)
                            .second);
        if(attach_notes) {
            unit.collected_diagnostics.push_back(std::move(diagnostic));
        }
    }

    void EndSourceFile() override {}

private:
    /// Formatting can deserialize declarations, and the AST reader reports
    /// the ODR mismatches it meets there as diagnostics of their own,
    /// reentrantly: the one converted here is built aside while theirs
    /// are recorded.
    Diagnostic convert(clang::DiagnosticsEngine::Level level, const clang::Diagnostic& raw) {
        Diagnostic diagnostic;
        diagnostic.id.value = raw.getID();
        diagnostic.id.level = diagnostic_level(level);
        diagnostic.id.source = DiagnosticSource::Clang;
        diagnostic.error_by_default =
            raw.getDiags()->getDiagnosticIDs()->isDefaultMappingAsError(raw.getID());

        /// TODO:
        // use DiagnosticEngine::SetArgToStringFn to set a custom function to convert arguments to
        // strings. Support markdown diagnostic in LSP 3.18. allow complex type to display in
        // markdown code block.
        ///
        /// auto& engine = src_mgr->getDiagnostics();
        /// engine.SetArgToStringFn();

        // clang blames the header for not being declared a header unit;
        // clice builds none, whatever the build declares.
        if(diagnostic.id.value == clang::diag::err_header_import_not_header_unit) {
            diagnostic.message = std::format("header units are not supported; {} is not imported",
                                             raw.getArgStdStr(0));
        } else {
            llvm::SmallString<256> message;
            raw.FormatDiagnostic(message);
            diagnostic.message = message.str();
        }

        if(auto pair = diagnostic_range(raw)) {
            auto [fid, range] = *pair;
            diagnostic.fid = fid;
            diagnostic.range = range;
            if(raw.hasSourceManager()) {
                diagnostic.in_system = raw.getSourceManager().isInSystemHeader(raw.getLocation());
            }
        }

        // One from the command line has no source to fix.
        if(raw.hasSourceManager()) {
            diagnostic.fix = fix(raw);
        }
        return diagnostic;
    }

    CompilationUnit& unit;

    /// Whether the notes that follow belong to a recorded diagnostic.
    bool attach_notes = false;

    /// The clang-tidy findings recorded, by place and message.
    std::set<std::tuple<clang::FileID, std::uint32_t, std::uint32_t, std::string>> findings;
};

std::unique_ptr<clang::DiagnosticConsumer> CompilationUnit::diagnostic_collector() {
    return std::make_unique<DiagnosticCollector>(*this);
}

}  // namespace clice
