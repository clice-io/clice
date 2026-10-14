module;

#include "modules/prelude.h"

module clice;

import :feature.feature;

namespace clice::feature {

namespace {

constexpr llvm::StringLiteral includer_context = "In includer context";

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

/// The page documenting a clang-tidy check. clang's own diagnostics have
/// a page listing many warning flags, but it says too little to be worth
/// linking.
std::optional<std::string> document_uri(const DiagnosticID& id) {
    if(id.source != DiagnosticSource::ClangTidy) {
        return std::nullopt;
    }
    // This won't correctly get the module for clang-analyzer checks, but as we
    // don't link in the analyzer that shouldn't be an issue.
    // This would also need updating if anyone decides to create a module with a
    // '-' in the name.
    auto [module, check] = id.name.split('-');
    if(module.empty() || check.empty()) {
        return std::nullopt;
    }
    return std::format("https://clang.llvm.org/extra/clang-tidy/checks/{}/{}.html", module, check);
}

bool is_deprecated(const DiagnosticID& id) {
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

    if(id.source == DiagnosticSource::ClangTidy) {
        return id.name.starts_with("modernize-");
    }
    return deprecated_diags.contains(id.value);
}

bool is_unused(const DiagnosticID& id) {
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

    if(id.source == DiagnosticSource::ClangTidy) {
        return id.name.starts_with("misc-unused-");
    }
    return unused_diags.contains(id.value);
}

class Presenter {
public:
    Presenter(CompilationUnit& unit, PositionEncoding encoding) :
        unit(unit), encoding(encoding), map(unit.focus_file().positions()) {}

    /// A diagnostic and the notes clang attached to it, as published on the
    /// main file; nullopt when it does not concern the main file.
    std::optional<protocol::Diagnostic> present(const Diagnostic& main) {
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
        if(main.fid.isValid() && !place(main, diagnostic)) {
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
        if(auto uri = document_uri(main.id)) {
            diagnostic.code_description = protocol::CodeDescription{.href = std::move(*uri)};
        }
        diagnostic.source = main.id.source == DiagnosticSource::ClangTidy ? "clang-tidy" : "clang";
        add_tag(diagnostic, main.id);
        for(const auto& note: main.notes) {
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
    /// takes part in it, a note, and to the top a fatal one, which silences
    /// everything after it, or a missing header, the likely cause of what
    /// goes wrong after it. Nothing else concerns the main file.
    bool place(const Diagnostic& main, protocol::Diagnostic& diagnostic) {
        auto focus = unit.focus_file();
        auto in_main = [&](const Diagnostic& note) {
            return focus.contains(note.fid);
        };
        if(focus.contains(main.fid)) {
            // A scope the main file leaves open swallows the includer's
            // remainder appended past its text: the error the buffer's end
            // raises shows where the scope opened.
            auto anchor = main.range;
            if(past_suffix_include(anchor.begin)) {
                auto note = llvm::find_if(main.notes, in_main);
                if(note == main.notes.end()) {
                    return false;
                }
                anchor = note->range;
            }
            auto range = map.range(anchor, encoding);
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
            auto request = llvm::find_if(main.notes, [&](const Diagnostic& note) {
                return in_main(note) && instantiation_note(note.id.value);
            });
            if(request != main.notes.end()) {
                anchor = request->range;
            } else if(in_context) {
                where = includer_context;
                if(auto note = llvm::find_if(main.notes, in_main); note != main.notes.end()) {
                    anchor = note->range;
                } else if(main.id.level == DiagnosticLevel::Fatal ||
                          main.id.value == clang::diag::err_pp_file_not_found) {
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
                auto range = map.range(*anchor, encoding);
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

        auto note = llvm::find_if(main.notes, in_main);
        if(note == main.notes.end()) {
            return false;
        }
        auto range = map.range(note->range, encoding);
        if(!range) {
            return false;
        }
        diagnostic.range = *range;
        return true;
    }

    /// Whether `offset` lies past the include of the includer's remainder
    /// that a borrowed context appends to the main file.
    bool past_suffix_include(std::uint32_t offset) {
        if(!unit.borrows_context()) {
            return false;
        }
        auto& includes = unit.directives()[unit.focus_file().file()].includes;
        return !includes.empty() && unit.synthesized(includes.back().fid) &&
               offset > unit.seen_at(includes.back().location).offset;
    }

    /// The filename of the #include in the main file that `fid` was
    /// entered through, directly or not.
    std::optional<LocalSourceRange> include_range(clang::FileID fid) {
        for(auto location = unit.include_location(fid); location.isValid();
            location = unit.include_location(fid)) {
            fid = unit.file_id(location);
            if(unit.focus_file().contains(fid)) {
                return unit.written_range(location)->range;
            }
        }
        return std::nullopt;
    }

    void add_tag(protocol::Diagnostic& diagnostic, DiagnosticID id) {
        if(is_deprecated(id)) {
            diagnostic.tags.emplace().push_back(protocol::DiagnosticTag::Deprecated);
        } else if(is_unused(id)) {
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

        auto converted = unit.positions(fid).range(range, encoding);
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

    CompilationUnit& unit;
    PositionEncoding encoding;
    PositionMap map;

    /// Where errors from other files were moved to.
    llvm::DenseSet<std::uint32_t> relocated;
};

}  // namespace

auto diagnostics(CompilationUnit& unit, PositionEncoding encoding)
    -> std::vector<protocol::Diagnostic> {
    Presenter presenter(unit, encoding);
    std::vector<protocol::Diagnostic> result;
    for(const auto& main: unit.diagnostics()) {
        if(auto diagnostic = presenter.present(main)) {
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

bool in_includer_context(const protocol::Diagnostic& diagnostic) {
    auto* message = std::get_if<std::string>(&diagnostic.message);
    return message && llvm::StringRef(*message).starts_with(includer_context);
}

}  // namespace clice::feature
