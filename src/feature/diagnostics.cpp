module;

#include "support/prelude.h"

module clice;

import :compile.diagnostic;
import :feature.diagnostics;
import :feature.protocol;
import :support.json;
import :vfs.path;

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

/// The page documenting a clang-tidy check. clang's own diagnostics have
/// a page listing many warning flags, but it says too little to be worth
/// linking.
std::string document_uri(const DiagnosticID& id) {
    if(id.source != DiagnosticSource::ClangTidy) {
        return {};
    }
    // This won't correctly get the module for clang-analyzer checks, but as we
    // don't link in the analyzer that shouldn't be an issue.
    // This would also need updating if anyone decides to create a module with a
    // '-' in the name.
    auto [module, check] = id.name.split('-');
    if(module.empty() || check.empty()) {
        return {};
    }
    return std::format("https://clang.llvm.org/extra/clang-tidy/checks/{}/{}.html", module, check);
}

/// Code that uses something deprecated, which an editor strikes through.
/// A clang-tidy check suggesting a modern spelling says nothing is.
bool is_deprecated(const FileDiagnostic& diagnostic) {
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

    switch(diagnostic.source) {
        case DiagnosticSource::Clang: return deprecated_diags.contains(diagnostic.id);
        case DiagnosticSource::ClangTidy:
            return llvm::StringRef(diagnostic.code).contains("-deprecated-");
        case DiagnosticSource::Clice: return false;
    }
    std::unreachable();
}

/// Code that does nothing, which an editor fades.
bool is_unused(const FileDiagnostic& diagnostic) {
    namespace diag = clang::diag;
    static llvm::DenseSet<std::uint32_t> unused_diags = {
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

    switch(diagnostic.source) {
        case DiagnosticSource::Clang: return unused_diags.contains(diagnostic.id);
        case DiagnosticSource::ClangTidy:
            return llvm::StringRef(diagnostic.code).starts_with("misc-unused-");
        case DiagnosticSource::Clice: return false;
    }
    std::unreachable();
}

class Presenter {
public:
    explicit Presenter(CompilationUnit& unit) : unit(unit) {}

    /// A diagnostic and the notes clang attached to it, as shown on the
    /// focus file; nullopt when it does not concern the focus file.
    std::optional<FileDiagnostic> present(const Diagnostic& main) {
        // The header itself is the focus file here; its own pragma is
        // exactly what makes it a system header for every includer.
        if(main.id.value == clang::diag::pp_pragma_sysheader_in_main_file &&
           unit.lang_options().IsHeaderFile) {
            return std::nullopt;
        }

        FileDiagnostic diagnostic{
            .level = main.id.level,
            .source = main.id.source,
            .id = main.id.source == DiagnosticSource::Clang ? main.id.value : 0,
            .code = main.id.diagnostic_code(),
            .link = document_uri(main.id),
            .message = main.message,
        };
        // One from the command line stays at the top of the file.
        if(main.fid.isValid() && !place(main, diagnostic)) {
            return std::nullopt;
        }
        diagnostic.range = in_text(diagnostic.range);
        for(const auto& note: main.notes) {
            add_note(diagnostic, note.fid, note.range, note.message);
        }
        return diagnostic;
    }

private:
    /// `range` of the focus file, ending with its text: a token running on
    /// past it (an unterminated raw string) runs into the suffix of a header
    /// context, which clang's buffer of the file goes on with.
    LocalSourceRange in_text(LocalSourceRange range) {
        auto size = static_cast<std::uint32_t>(unit.focus_file().content().size());
        return {std::min(range.begin, size), std::min(range.end, size)};
    }

    /// Locate the diagnostic in the focus file. One from another file moves
    /// there: to the instantiation the focus file requested, else an error
    /// to the #include or import that brought its file in, else to the top of the
    /// file — saying where it happened and pointing back at it; any other
    /// to its first note in the focus file. A borrowed includer context is the host's
    /// code: an error raised there moves to where the focus file takes part
    /// in it, a note, and to the top a fatal one, which silences
    /// everything after it, or a missing header, the likely cause of what
    /// goes wrong after it. Nothing else concerns the focus file.
    bool place(const Diagnostic& main, FileDiagnostic& diagnostic) {
        auto focus = unit.focus_file();
        auto in_focus = [&](const Diagnostic& note) {
            return focus.contains(note.fid);
        };
        if(focus.contains(main.fid)) {
            // A scope the focus file leaves open swallows the include of a
            // header context's suffix clang's buffer goes on with past the
            // text: the error the buffer's end raises shows where the scope
            // opened.
            auto anchor = main.range;
            if(anchor.begin > focus.content().size()) {
                auto note = ranges::find_if(main.notes, in_focus);
                if(note == main.notes.end()) {
                    return false;
                }
                anchor = note->range;
            }
            diagnostic.range = anchor;
            return true;
        }

        auto request = ranges::find_if(main.notes, [&](const Diagnostic& note) {
            return in_focus(note) && instantiation_note(note.id.value);
        });
        if(request != main.notes.end()) {
            return relocate(main, diagnostic, request->range, DiagnosticOrigin::Template);
        }
        bool in_context = unit.from_context(main.fid);
        if(main.error_by_default) {
            if(in_context) {
                if(auto note = ranges::find_if(main.notes, in_focus); note != main.notes.end()) {
                    return relocate(main,
                                    diagnostic,
                                    note->range,
                                    DiagnosticOrigin::IncluderContext);
                }
                if(main.id.level == DiagnosticLevel::Fatal ||
                   main.id.value == clang::diag::err_pp_file_not_found) {
                    return relocate(main, diagnostic, {0, 0}, DiagnosticOrigin::IncluderContext);
                }
            } else if(auto include = include_range(main.fid)) {
                // A module unit's files are entered at the import that
                // loaded its module.
                auto& sources = unit.context().getSourceManager();
                bool imported = sources.getModuleImportLoc(sources.getLocForStartOfFile(main.fid))
                                    .first.isValid();
                return relocate(main,
                                diagnostic,
                                *include,
                                imported ? DiagnosticOrigin::ImportedModule
                                         : DiagnosticOrigin::IncludedFile);
            } else {
                // What the command line brought in (-include, -D) sits on
                // no #include: it stays at the top of the file, like the
                // command line's own diagnostics.
                return relocate(main,
                                diagnostic,
                                {0, 0},
                                unit.is_builtin_file(main.fid) ? DiagnosticOrigin::File
                                                               : DiagnosticOrigin::IncludedFile);
            }
        }
        if(in_context) {
            return false;
        }

        auto note = ranges::find_if(main.notes, in_focus);
        if(note == main.notes.end()) {
            return false;
        }
        diagnostic.range = note->range;
        return true;
    }

    /// Show a diagnostic of another file at `anchor` in the focus file.
    bool relocate(const Diagnostic& main,
                  FileDiagnostic& diagnostic,
                  LocalSourceRange anchor,
                  DiagnosticOrigin origin) {
        diagnostic.range = anchor;
        diagnostic.origin = origin;
        add_note(diagnostic,
                 main.fid,
                 main.range,
                 main.id.level >= DiagnosticLevel::Error ? "error occurred here"
                                                         : "warning occurred here");
        return true;
    }

    /// The filename of the #include, or the import, in the focus file that
    /// `fid` was entered through, directly or not.
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

    void add_note(FileDiagnostic& diagnostic,
                  clang::FileID fid,
                  LocalSourceRange range,
                  llvm::StringRef message) {
        /// A note can point into a synthetic buffer, e.g. a macro defined
        /// on the command line, or into a header context's synthesized
        /// fragment; there is no file to link to.
        if(fid.isInvalid() || unit.is_builtin_file(fid) || unit.synthesized(fid) ||
           !range.valid()) {
            return;
        }
        bool own = unit.focus_file().contains(fid);
        diagnostic.notes.push_back({
            .path = own ? std::string() : unit.file_path(fid).str(),
            .range = own ? in_text(range) : range,
            .message = message.str(),
        });
    }

    CompilationUnit& unit;
};

}  // namespace

auto diagnostics(CompilationUnit& unit) -> std::vector<FileDiagnostic> {
    std::vector<FileDiagnostic> result;
    // A compile that failed before parsing has no text to place what it
    // reports in: why it failed shows at the top of the file.
    if(unit.setup_fail()) {
        for(const auto& main: unit.diagnostics()) {
            if(main.id.level >= DiagnosticLevel::Error) {
                result.push_back({
                    .level = main.id.level,
                    .source = main.id.source,
                    .id = main.id.value,
                    .code = main.id.diagnostic_code(),
                    .message = main.message,
                });
            }
        }
        return result;
    }
    Presenter presenter(unit);
    for(const auto& main: unit.diagnostics()) {
        if(auto diagnostic = presenter.present(main)) {
            result.push_back(std::move(*diagnostic));
        }
    }
    return result;
}

auto skipped_diagnostics(CompilationUnit& unit) -> std::vector<FileDiagnostic> {
    std::vector<FileDiagnostic> result;
    auto focus = unit.focus_file();
    Presenter presenter(unit);
    for(const auto& main: unit.diagnostics()) {
        bool skippable = !focus.contains(main.fid) ||
                         ranges::any_of(main.notes,
                                        [](const Diagnostic& note) {
                                            return instantiation_note(note.id.value);
                                        }) ||
                         (main.id.source == DiagnosticSource::Clang &&
                          ranges::contains(unused_declaration_warnings, main.id.value));
        if(!skippable) {
            continue;
        }
        if(auto diagnostic = presenter.present(main)) {
            result.push_back(std::move(*diagnostic));
        }
    }
    return result;
}

FileDiagnostic file_warning(std::string message) {
    return {.message = std::move(message)};
}

/// Clang diagnostics indicating that an input file could not be found —
/// the symptom of a guessed compile command missing include paths.
static bool is_file_not_found(const FileDiagnostic& diagnostic) {
    return diagnostic.origin != DiagnosticOrigin::IncluderContext &&
           (diagnostic.id == clang::diag::err_pp_file_not_found ||
            diagnostic.id == clang::diag::err_pp_error_opening_file ||
            diagnostic.id == clang::diag::err_module_not_found);
}

auto shown_diagnostics(const CompileDiagnostics& compiled, const DiagnosticsOptions& options)
    -> std::vector<FileDiagnostic> {
    // Of the diagnostics moved from other files to one spot, the first
    // error and the first warning show: a broken header's every error
    // would pile up on its #include.
    std::set<std::pair<std::uint32_t, bool>> relocated;
    auto diagnostics = compiled.own;
    diagnostics.append_range(compiled.skipped);
    std::erase_if(diagnostics, [&](const FileDiagnostic& diagnostic) {
        return ranges::contains(options.suppress, diagnostic.code) ||
               (diagnostic.origin != DiagnosticOrigin::File &&
                !relocated
                     .insert({diagnostic.range.begin, diagnostic.level >= DiagnosticLevel::Error})
                     .second);
    });

    auto warn = [&](llvm::StringRef code, std::string message, llvm::StringRef link = {}) {
        if(!ranges::contains(options.suppress, code)) {
            auto warning = file_warning(std::move(message));
            warning.code = code.str();
            warning.link = link.str();
            diagnostics.insert(diagnostics.begin(), std::move(warning));
        }
    };

    // Guidance (and only when it can explain something): an exact CDB match
    // never gets the note, and neither does a guessed command that worked.
    // The includer's own code compiles under its own command: what it
    // misses, no command for the file would find.
    auto source = compiled.source;
    if(source != CommandSource::CDBExact && source != CommandSource::Default &&
       ranges::any_of(diagnostics, is_file_not_found)) {
        warn("inferred-compile-command",
             std::format("No compilation database entry for this file (compile command was {}), "
                         "so some includes may not be found. Configure compile_commands.json for "
                         "accurate diagnostics.",
                         source == CommandSource::Fallback ? "synthesized from defaults"
                         : source == CommandSource::Inferred
                             ? "borrowed from a nearby translation unit"
                             : "inferred from an including file"),
             "https://docs.clice.io/clice/guide/quick-start#project-setup");
    }
    // The includer context the header needed but could not get: the
    // diagnostics below miss what it provides.
    if(!compiled.unmatched_host.empty()) {
        warn("unmatched-includer-context",
             std::format("This header needs the context of its includer, but the include chain "
                         "from {} could not be followed under that file's compile command, so it "
                         "compiled without it.",
                         compiled.unmatched_host));
    }
    // clang's own driver read the command: what the compiler sets up itself
    // (system headers, macros, the default standard) is clang's idea of it.
    if(!compiled.query_failure.empty()) {
        warn("toolchain-query-failed",
             std::format("The compiler of this file's compile command could not be queried ({}), "
                         "so clang read the command on its own: system headers, predefined macros "
                         "and the default standard may differ from the compiler's.",
                         compiled.query_failure));
    }
    return diagnostics;
}

auto with_preamble(std::vector<FileDiagnostic> own, std::vector<FileDiagnostic> preamble)
    -> std::vector<FileDiagnostic> {
    if(preamble.empty()) {
        return own;
    }
    llvm::StringSet<> raised;
    for(auto& diagnostic: own) {
        raised.insert(to_client_json(diagnostic, ""));
    }
    std::erase_if(preamble, [&](const FileDiagnostic& diagnostic) {
        return raised.contains(to_client_json(diagnostic, ""));
    });
    ranges::move(own, std::back_inserter(preamble));
    return preamble;
}

auto take_unconfirmed(std::vector<FileDiagnostic>& diagnostics) -> std::vector<FileDiagnostic> {
    auto unused = [](const FileDiagnostic& diagnostic) {
        return ranges::contains(unused_declaration_warnings, diagnostic.id);
    };
    std::vector<FileDiagnostic> unconfirmed;
    ranges::copy_if(diagnostics, std::back_inserter(unconfirmed), unused);
    std::erase_if(diagnostics, unused);
    return unconfirmed;
}

bool same_diagnostic(const FileDiagnostic& left, const FileDiagnostic& right) {
    return left.range.begin == right.range.begin && left.message == right.message;
}

auto confirmed_skipped(llvm::ArrayRef<FileDiagnostic> skipped, const CompileDiagnostics& compiled)
    -> std::vector<FileDiagnostic> {
    std::vector<FileDiagnostic> result;
    for(const auto& diagnostic: skipped) {
        auto same = [&](const FileDiagnostic& own) {
            return same_diagnostic(own, diagnostic);
        };
        if(ranges::contains(unused_declaration_warnings, diagnostic.id)) {
            if(auto own = ranges::find_if(compiled.unconfirmed, same);
               own != compiled.unconfirmed.end()) {
                result.push_back(*own);
            }
        } else if(ranges::none_of(compiled.own, same)) {
            result.push_back(diagnostic);
        }
    }
    return result;
}

void explain_failed_imports(std::vector<FileDiagnostic>& diagnostics,
                            llvm::ArrayRef<ModuleFailure> failures) {
    for(const auto& failure: failures) {
        llvm::ArrayRef<std::string> chain = failure.chain;
        std::string message;
        for(const auto& module: chain.take_front(failure.cycle.value_or(chain.size()))) {
            message += std::format("could not build module '{}': ", module);
        }
        if(failure.cycle) {
            auto cycle = chain.drop_front(*failure.cycle);
            message += std::format("cyclic dependency in module '{}': ", cycle.front());
            for(const auto& module: cycle) {
                message += std::format("{} -> ", module);
            }
            message += cycle.front();
        } else {
            message += failure.error;
        }
        auto not_found = std::format("module '{}' not found", chain.front());
        for(auto& diagnostic: diagnostics) {
            if(diagnostic.id == clang::diag::err_module_not_found &&
               diagnostic.message == not_found) {
                diagnostic.message = message;
                // As clang codes it: a cycle only where the imported module
                // is on it, not built where the message starts with one
                // that is not.
                diagnostic.id = failure.cycle == 0 ? clang::diag::err_module_cycle
                                                   : clang::diag::err_module_not_built;
                diagnostic.code = failure.cycle == 0 ? "err_module_cycle" : "err_module_not_built";
            }
        }
    }
}

FileDiagnostic ambiguous_module(const ModuleChoice& choice, llvm::StringRef text) {
    auto warning = file_warning(
        std::format("module '{}' is declared by more than one file; this file imports {}, the "
                    "nearest, not {}",
                    choice.module,
                    choice.chosen,
                    llvm::join(choice.others, ", ")));
    warning.code = "ambiguous-module";
    // At the import when it is the file's own; an import through an
    // included file sits at the include, one of the command line's or
    // the implicit one of an implementation unit at the top.
    if(choice.offset < text.size()) {
        auto end = std::min(text.find('\n', choice.offset), text.size());
        warning.range = {choice.offset, static_cast<std::uint32_t>(end)};
    }
    return warning;
}

FileDiagnostic invalid_tidy_config(llvm::StringRef error) {
    auto warning = file_warning(
        std::format("clang-tidy does not run on this file until its configuration parses. {}",
                    error));
    warning.code = "invalid-clang-tidy-config";
    return warning;
}

FileDiagnostic index_unavailable(llvm::StringRef cause) {
    return file_warning(std::format(
        "{}; features that read the file's own index, such as references within it, are "
        "unavailable",
        cause));
}

protocol::Diagnostic to_protocol(
    const FileDiagnostic& diagnostic,
    bool related_information,
    PositionEncoding encoding,
    llvm::StringRef path,
    const PositionMap& positions,
    llvm::function_ref<std::optional<PositionMap>(llvm::StringRef path)> note_positions) {
    protocol::Diagnostic result{.range = *positions.range(diagnostic.range, encoding),
                                .message = {}};
    switch(diagnostic.level) {
        case DiagnosticLevel::Note:
        case DiagnosticLevel::Remark:
            result.severity = protocol::DiagnosticSeverity::Information;
            break;
        case DiagnosticLevel::Warning:
            result.severity = protocol::DiagnosticSeverity::Warning;
            break;
        case DiagnosticLevel::Error:
        case DiagnosticLevel::Fatal: result.severity = protocol::DiagnosticSeverity::Error; break;
    }
    if(!diagnostic.code.empty()) {
        result.code = diagnostic.code;
    }
    if(!diagnostic.link.empty()) {
        result.code_description = protocol::CodeDescription{.href = diagnostic.link};
    }
    switch(diagnostic.source) {
        case DiagnosticSource::Clang: result.source = "clang"; break;
        case DiagnosticSource::ClangTidy: result.source = "clang-tidy"; break;
        case DiagnosticSource::Clice: result.source = "clice"; break;
    }
    if(is_deprecated(diagnostic)) {
        result.tags.emplace().push_back(protocol::DiagnosticTag::Deprecated);
    } else if(is_unused(diagnostic)) {
        result.tags.emplace().push_back(protocol::DiagnosticTag::Unnecessary);
    }

    std::string message;
    switch(diagnostic.origin) {
        case DiagnosticOrigin::File: break;
        case DiagnosticOrigin::Template: message = "In template: "; break;
        case DiagnosticOrigin::IncludedFile: message = "In included file: "; break;
        case DiagnosticOrigin::ImportedModule: message = "In imported module: "; break;
        case DiagnosticOrigin::IncluderContext: message = "In includer context: "; break;
    }
    message += diagnostic.message;
    std::vector<protocol::DiagnosticRelatedInformation> related;
    for(const auto& note: diagnostic.notes) {
        auto file = note.path.empty() ? path : llvm::StringRef(note.path);
        auto map = note.path.empty() ? std::optional(positions) : note_positions(note.path);
        if(!map || note.range.end > map->size()) {
            continue;
        }
        auto range = *map->range(note.range, encoding);
        if(related_information) {
            related.push_back({
                .location = {.uri = to_uri(file), .range = range},
                .message = note.message,
            });
        } else {
            message += std::format("\n\n{}:{}:{}: {}",
                                   path::filename(file),
                                   range.start.line + 1,
                                   range.start.character + 1,
                                   note.message);
        }
    }
    if(!related.empty()) {
        result.related_information = std::move(related);
    }
    result.message = std::move(message);
    return result;
}

}  // namespace clice::feature
