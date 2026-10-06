#include <format>
#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"

#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/DenseSet.h"
#include "clang/Basic/DiagnosticLex.h"
#include "clang/Basic/DiagnosticSema.h"

namespace clice::feature {

namespace {

/// The notes of a template instantiation backtrace: each names the code
/// that requested the instantiation the diagnostic arose in.
bool instantiation_note(std::uint32_t id) {
    namespace diag = clang::diag;
    switch(id) {
        case diag::note_template_class_instantiation_was_here:
        case diag::note_template_class_explicit_specialization_was_here:
        case diag::note_template_class_instantiation_here:
        case diag::note_template_member_class_here:
        case diag::note_template_member_function_here:
        case diag::note_function_template_spec_here:
        case diag::note_template_static_data_member_def_here:
        case diag::note_template_variable_def_here:
        case diag::note_template_enum_def_here:
        case diag::note_template_nsdmi_here:
        case diag::note_template_type_alias_instantiation_here:
        case diag::note_template_exception_spec_instantiation_here:
        case diag::note_template_requirement_instantiation_here:
        case diag::note_evaluating_exception_spec_here:
        case diag::note_default_arg_instantiation_here:
        case diag::note_default_function_arg_instantiation_here:
        case diag::note_explicit_template_arg_substitution_here:
        case diag::note_function_template_deduction_instantiation_here:
        case diag::note_deduced_template_arg_substitution_here:
        case diag::note_prior_template_arg_substitution:
        case diag::note_template_default_arg_checking:
        case diag::note_concept_specialization_here:
        case diag::note_nested_requirement_here:
        case diag::note_checking_constraints_for_template_id_here:
        case diag::note_checking_constraints_for_var_spec_id_here:
        case diag::note_checking_constraints_for_class_spec_id_here:
        case diag::note_checking_constraints_for_function_here:
        case diag::note_constraint_substitution_here:
        case diag::note_parameter_mapping_substitution_here: return true;
        default: return false;
    }
}

bool is_note(const Diagnostic& diagnostic) {
    return diagnostic.id.level == DiagnosticLevel::Note ||
           diagnostic.id.level == DiagnosticLevel::Remark;
}

class Presenter {
public:
    Presenter(CompilationUnitRef unit, PositionEncoding encoding) :
        unit(unit), encoding(encoding), map(main_position_map(unit, encoding)) {}

    /// A diagnostic and the notes clang attached to it, as published on the
    /// main file; nullopt when it does not concern the main file.
    std::optional<protocol::Diagnostic> present(const Diagnostic& main,
                                                llvm::ArrayRef<Diagnostic> notes) {
        if(main.id.level == DiagnosticLevel::Ignored) {
            return std::nullopt;
        }
        // The header itself is the main file here; its own pragma is
        // exactly what makes it a system header for every includer.
        if(main.id.value == clang::diag::pp_pragma_sysheader_in_main_file &&
           unit.lang_options().IsHeaderFile) {
            return std::nullopt;
        }

        protocol::Diagnostic diagnostic{
            .range = {},
            .message = main.message,
        };
        // One from the command line stays at the top of the file.
        if(main.fid.isValid() && !place(main, notes, diagnostic)) {
            return std::nullopt;
        }

        if(main.id.level == DiagnosticLevel::Warning) {
            diagnostic.severity = protocol::DiagnosticSeverity::Warning;
        } else if(main.id.level == DiagnosticLevel::Error ||
                  main.id.level == DiagnosticLevel::Fatal) {
            diagnostic.severity = protocol::DiagnosticSeverity::Error;
        }
        if(auto code = main.id.diagnostic_code(); !code.empty()) {
            diagnostic.code = code.str();
        }
        if(auto uri = main.id.diagnostic_document_uri()) {
            diagnostic.code_description = protocol::CodeDescription{.href = std::move(*uri)};
        }
        diagnostic.source = diagnostic_source_name(main.id.source).str();
        add_tag(diagnostic, main.id);
        for(const auto& note: notes) {
            add_related(diagnostic, note.fid, note.range, note.message);
        }
        return diagnostic;
    }

private:
    /// Locate the diagnostic in the main file. One from another file moves
    /// there: an error to the instantiation the main file requested, else
    /// to the #include that brought its file in, else to the top of the
    /// file — prefixed with where it happened and pointing back at it, only
    /// the first of the errors landing on one spot kept; any other to its
    /// first note in the main file. A borrowed includer context is the
    /// host's code: an error raised there moves to where the main file
    /// takes part in it, a note, and a fatal one, which silences everything
    /// after it, to the top. Nothing else concerns the main file.
    bool place(const Diagnostic& main,
               llvm::ArrayRef<Diagnostic> notes,
               protocol::Diagnostic& diagnostic) {
        auto in_main = [&](const Diagnostic& note) {
            return note.fid == unit.main_file();
        };
        if(main.fid == unit.main_file()) {
            // A scope the main file leaves open swallows the includer's
            // remainder appended past its text: the error the buffer's end
            // raises shows where the scope opened.
            auto anchor = main.range;
            if(past_suffix_include(anchor.begin)) {
                auto note = llvm::find_if(notes, in_main);
                if(note == notes.end()) {
                    return false;
                }
                anchor = note->range;
            }
            auto range = map.to_range(anchor);
            if(!range) {
                return false;
            }
            diagnostic.range = *range;
            return true;
        }

        bool in_context = unit.from_context(main.fid);
        if(main.error_by_default) {
            std::optional<LocalSourceRange> anchor;
            std::string_view where = "In template";
            auto request = llvm::find_if(notes, [&](const Diagnostic& note) {
                return in_main(note) && instantiation_note(note.id.value);
            });
            if(request != notes.end()) {
                anchor = request->range;
            } else if(in_context) {
                where = "In includer context";
                if(auto note = llvm::find_if(notes, in_main); note != notes.end()) {
                    anchor = note->range;
                } else if(main.id.level == DiagnosticLevel::Fatal) {
                    anchor = LocalSourceRange{0, 0};
                }
            } else {
                anchor = include_range(main.fid);
                where = "In included file";
                // What the command line brought in (-include, -D) sits on
                // no #include: it stays at the top of the file, like the
                // command line's own diagnostics.
                if(!anchor) {
                    anchor = LocalSourceRange{0, 0};
                    if(unit.is_builtin_file(main.fid)) {
                        where = {};
                    }
                }
            }
            if(anchor) {
                auto range = map.to_range(*anchor);
                if(!range || !relocated.insert(anchor->begin).second) {
                    return false;
                }
                diagnostic.range = *range;
                if(!where.empty()) {
                    diagnostic.message = std::format("{}: {}", where, main.message);
                }
                add_related(diagnostic, main.fid, main.range, "error occurred here");
                return true;
            }
        }
        if(in_context) {
            return false;
        }

        auto note = llvm::find_if(notes, in_main);
        if(note == notes.end()) {
            return false;
        }
        auto range = map.to_range(note->range);
        if(!range) {
            return false;
        }
        diagnostic.range = *range;
        return true;
    }

    /// The filename of the #include in the main file that `fid` was
    /// entered through, directly or not.
    /// Whether `offset` lies past the include of the includer's remainder
    /// that a borrowed context appends to the main file.
    bool past_suffix_include(std::uint32_t offset) {
        if(!unit.borrows_context()) {
            return false;
        }
        auto& includes = unit.directives()[unit.main_file()].includes;
        return !includes.empty() && unit.synthesized(includes.back().fid) &&
               offset > unit.file_offset(includes.back().location);
    }

    std::optional<LocalSourceRange> include_range(clang::FileID fid) {
        for(auto location = unit.include_location(fid); location.isValid();
            location = unit.include_location(fid)) {
            fid = unit.file_id(location);
            if(unit.is_main_file(fid)) {
                auto offset = unit.file_offset(location);
                return LocalSourceRange{offset, offset + unit.token_length(location)};
            }
        }
        return std::nullopt;
    }

    void add_tag(protocol::Diagnostic& diagnostic, DiagnosticID id) {
        if(id.is_deprecated()) {
            diagnostic.tags.emplace().push_back(protocol::DiagnosticTag::Deprecated);
        } else if(id.is_unused()) {
            diagnostic.tags.emplace().push_back(protocol::DiagnosticTag::Unnecessary);
        }
    }

    void add_related(protocol::Diagnostic& diagnostic,
                     clang::FileID fid,
                     LocalSourceRange range,
                     llvm::StringRef message) {
        /// Related information can point into a synthetic buffer, e.g. a
        /// macro defined on the command line, or into a header context's
        /// synthesized fragment; there is no file to link to.
        if(fid.isInvalid() || unit.is_builtin_file(fid) || unit.synthesized(fid) ||
           !range.valid()) {
            return;
        }

        std::optional<protocol::Range> converted;
        if(fid == unit.main_file()) {
            converted = map.to_range(range);
        } else {
            auto content = unit.file_content(fid);
            auto lines = lsp::line_starts(content);
            converted =
                PositionMap{.content = content, .lines = lines, .encoding = encoding}.to_range(
                    range);
        }
        if(!converted) {
            return;
        }

        if(!diagnostic.related_information.has_value()) {
            diagnostic.related_information = std::vector<protocol::DiagnosticRelatedInformation>();
        }
        diagnostic.related_information->push_back({
            .location =
                protocol::Location{
                                   .uri = to_uri(unit.file_path(fid)),
                                   .range = *converted,
                                   },
            .message = message.str(),
        });
    }

    CompilationUnitRef unit;
    PositionEncoding encoding;
    PositionMap map;

    /// Where errors from other files were moved to.
    llvm::DenseSet<std::uint32_t> relocated;
};

}  // namespace

auto diagnostics(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::Diagnostic> {
    Presenter presenter(unit, encoding);
    std::vector<protocol::Diagnostic> result;
    llvm::ArrayRef<Diagnostic> raw = unit.diagnostics();
    // Notes follow the diagnostic they belong to; one ahead of every
    // diagnostic belongs to none.
    while(!raw.empty()) {
        const auto& main = raw.front();
        auto notes = raw.drop_front().take_while(is_note);
        raw = raw.drop_front(notes.size() + 1);
        if(is_note(main)) {
            continue;
        }
        if(auto diagnostic = presenter.present(main, notes)) {
            result.push_back(std::move(*diagnostic));
        }
    }
    return result;
}

protocol::Diagnostic file_warning(std::string message) {
    protocol::Diagnostic diagnostic;
    diagnostic.range = protocol::Range{
        .start = protocol::Position{.line = 0, .character = 0},
        .end = protocol::Position{.line = 0, .character = 0},
    };
    diagnostic.severity = protocol::DiagnosticSeverity::Warning;
    diagnostic.source = "clice";
    diagnostic.message = std::move(message);
    return diagnostic;
}

}  // namespace clice::feature
