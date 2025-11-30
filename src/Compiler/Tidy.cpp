//===--- Run clang-tidy ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// Partial code is copied from clangd. See:
/// https://github.com/llvm/llvm-project//blob/0865ecc5150b9a55ba1f9e30b6d463a66ac362a6/clang-tools-extra/clangd/ParsedAST.cpp#L547
/// https://github.com/llvm/llvm-project//blob/0865ecc5150b9a55ba1f9e30b6d463a66ac362a6/clang-tools-extra/clangd/TidyProvider.cpp

#include "Compiler/Tidy.h"

#include "TidyImpl.h"
#include "AST/Utility.h"
#include "Compiler/Diagnostic.h"
#include "Support/Logging.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/StringSaver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyDiagnosticConsumer.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang-tidy/ClangTidyOptions.h"
#define CLANG_TIDY_DISABLE_STATIC_ANALYZER_CHECKS
#include "clang-tidy/ClangTidyForceLinker.h"

namespace clice::tidy {

using namespace clang::tidy;

bool is_registered_tidy_check(llvm::StringRef check) {
    assert(!check.empty());
    assert(!check.contains('*') && !check.contains(',') &&
           "is_registered_tidy_check doesn't support globs");
    assert(check.ltrim().front() != '-');

    const static llvm::StringSet<llvm::BumpPtrAllocator> all_checks = [] {
        llvm::StringSet<llvm::BumpPtrAllocator> result;
        tidy::ClangTidyCheckFactories factories;
        for(tidy::ClangTidyModuleRegistry::entry entry: tidy::ClangTidyModuleRegistry::entries())
            entry.instantiate()->addCheckFactories(factories);
        for(const auto& factory: factories)
            result.insert(factory.getKey());
        return result;
    }();

    return all_checks.contains(check);
}

std::optional<bool> is_fast_tidy_check(llvm::StringRef check) {
    static auto fast = llvm::StringMap<bool>{
#define FAST(CHECK, TIME) {#CHECK, true},
#define SLOW(CHECK, TIME) {#CHECK, false},
// todo: move me to llvm toolchain headers.
#include "TidyFastChecks.inc"
    };

    if(auto it = fast.find(check); it != fast.end()) {
        return it->second;
    }
    return std::nullopt;
}

tidy::ClangTidyCheckFactories get_fast_checks(const tidy::ClangTidyCheckFactories& all) {
    tidy::ClangTidyCheckFactories fast;
    for(const auto& factory: all) {
        if(is_fast_tidy_check(factory.getKey()).value_or(false)) {
            fast.registerCheckFactory(factory.first(), factory.second);
        }
    }
    return fast;
}

tidy::ClangTidyOptions create_options() {
    // getDefaults instantiates all check factories, which are registered at link
    // time. So cache the results once.
    const static auto default_opts = [] {
        auto opts = tidy::ClangTidyOptions::getDefaults();
        opts.Checks->clear();
        return opts;
    }();
    // These default checks are chosen for:
    //  - low false-positive rate
    //  - providing a lot of value
    //  - being reasonably efficient
    const static std::string default_checks = llvm::join_items(",",
                                                               "readability-misleading-indentation",
                                                               "readability-deleted-default",
                                                               "bugprone-integer-division",
                                                               "bugprone-sizeof-expression",
                                                               "bugprone-suspicious-missing-comma",
                                                               "bugprone-unused-raii",
                                                               "bugprone-unused-return-value",
                                                               "misc-unused-using-decls",
                                                               "misc-unused-alias-decls",
                                                               "misc-definitions-in-headers");
    const static std::string bad_checks =
        llvm::join_items(",",
                         // We want this list to start with a separator to
                         // simplify appending in the lambda. So including an
                         // empty string here will force that.
                         "",
                         // include-cleaner is directly integrated in IncludeCleaner.cpp
                         "-misc-include-cleaner",

                         // ----- False Positives -----

                         // Check relies on seeing ifndef/define/endif directives,
                         // clangd doesn't replay those when using a preamble.
                         "-llvm-header-guard",
                         "-modernize-macro-to-enum",

                         // ----- Crashing Checks -----

                         // Check can choke on invalid (intermediate) c++
                         // code, which is often the case when clangd
                         // tries to build an AST.
                         "-bugprone-use-after-move",
                         // Alias for bugprone-use-after-move.
                         "-hicpp-invalid-access-moved",
                         // Check uses dataflow analysis, which might hang/crash unexpectedly on
                         // incomplete code.
                         "-bugprone-unchecked-optional-access");

    tidy::ClangTidyOptions opts = default_opts;

    // clang::clangd::provideEnvironment
    if(std::optional<std::string> user = llvm::sys::Process::GetEnv("USER")) {
        opts.User = user;
    }
    // TODO: Providers.push_back(provideClangTidyFiles(TFS)); Filename
    // TODO: if(EnableConfig) Providers.push_back(provideClangdConfig());
    // clang::clangd::provideDefaultChecks
    if(!opts.Checks || opts.Checks->empty()) {
        opts.Checks = default_checks;
    }
    // clang::clangd::disableUnusableChecks
    if(opts.Checks && !opts.Checks->empty()) {
        opts.Checks->append(bad_checks);
    }
    return opts;
}

// Filter for clang diagnostics groups enabled by CTOptions.Checks.
//
// These are check names like clang-diagnostics-unused.
// Note that unlike -Wunused, clang-diagnostics-unused does not imply
// subcategories like clang-diagnostics-unused-function.
//
// This is used to determine which diagnostics can be enabled by ExtraArgs in
// the clang-tidy configuration.
class TidyDiagnosticGroups {
    // Whether all diagnostic groups are enabled by default.
    // True if we've seen clang-diagnostic-*.
    bool default_enable = false;
    // Set of diag::Group whose enablement != default_enable.
    // If default_enable is false, this is foo where we've seen clang-diagnostic-foo.
    llvm::DenseSet<unsigned> exceptions;

public:
    TidyDiagnosticGroups(llvm::StringRef checks) {
        constexpr llvm::StringLiteral CDPrefix = "clang-diagnostic-";

        llvm::StringRef check;
        while(!checks.empty()) {
            std::tie(check, checks) = checks.split(',');
            check = check.trim();

            if(check.empty()) {
                continue;
            }

            bool enable = !check.consume_front("-");
            bool glob = check.consume_back("*");
            if(glob) {
                // Is this clang-diagnostic-*, or *, or so?
                // (We ignore all other types of globs).
                if(CDPrefix.starts_with(check)) {
                    default_enable = enable;
                    exceptions.clear();
                }
                continue;
            }

            // In "*,clang-diagnostic-foo", the latter is a no-op.
            if(default_enable == enable) {
                continue;
            }
            // The only non-glob entries we care about are clang-diagnostic-foo.
            if(!check.consume_front(CDPrefix)) {
                continue;
            }

            if(auto group = clang::DiagnosticIDs::getGroupForWarningOption(check)) {
                exceptions.insert(static_cast<unsigned>(*group));
            }
        }
    }

    bool operator()(clang::diag::Group group_id) const {
        return exceptions.contains(static_cast<unsigned>(group_id)) ? !default_enable
                                                                    : default_enable;
    }
};

// Find -W<group> and -Wno-<group> options in extra_args and apply them to diags.
//
// This is used to handle extra_args in clang-tidy configuration.
// We don't use clang's standard handling of this as we want slightly different
// behavior (e.g. we want to exclude these from -Wno-error).
void apply_warning_options(llvm::ArrayRef<std::string> extra_args,
                           llvm::function_ref<bool(clang::diag::Group)> enable_groups,
                           clang::DiagnosticsEngine& diags) {
    for(llvm::StringRef group: extra_args) {
        // Only handle args that are of the form -W[no-]<group>.
        // Other flags are possible but rare and deliberately out of scope.
        llvm::SmallVector<clang::diag::kind> members;
        if(!group.consume_front("-W") || group.empty()) {
            continue;
        }
        bool enable = !group.consume_front("no-");
        if(diags.getDiagnosticIDs()->getDiagnosticsInGroup(clang::diag::Flavor::WarningOrError,
                                                           group,
                                                           members)) {
            continue;
        }

        // Upgrade (or downgrade) the severity of each diagnostic in the group.
        // If -Werror is on, newly added warnings will be treated as errors.
        // We don't want this, so keep track of them to fix afterwards.
        bool needs_werror_exclusion = false;
        for(clang::diag::kind id: members) {
            if(enable) {
                if(diags.getDiagnosticLevel(id, clang::SourceLocation()) <
                   clang::DiagnosticsEngine::Warning) {
                    auto group = diags.getDiagnosticIDs()->getGroupForDiag(id);
                    if(!group || !enable_groups(*group)) {
                        continue;
                    }
                    diags.setSeverity(id, clang::diag::Severity::Warning, clang::SourceLocation());
                    if(diags.getWarningsAsErrors()) {
                        needs_werror_exclusion = true;
                    }
                }
            } else {
                diags.setSeverity(id, clang::diag::Severity::Ignored, clang::SourceLocation());
            }
        }
        if(needs_werror_exclusion) {
            // FIXME: there's no API to suppress -Werror for single diagnostics.
            // In some cases with sub-groups, we may end up erroneously
            // downgrading diagnostics that were -Werror in the compile command.
            diags.setDiagnosticGroupWarningAsError(group, false);
        }
    }
}

ClangTidyChecker::ClangTidyChecker(std::unique_ptr<ClangTidyOptionsProvider> provider) :
    context(std::move(provider)) {}

clang::DiagnosticsEngine::Level
    ClangTidyChecker::adjust_level(clang::DiagnosticsEngine::Level level,
                                   const clang::Diagnostic& diag) {
    if(!checks.empty()) {
        std::string tidy_diag = context.getCheckName(diag.getID());
        bool is_clang_tidy_diag = !tidy_diag.empty();
        if(is_clang_tidy_diag) {
            // Check for suppression comment. Skip the check for diagnostics not
            // in the main file, because we don't want that function to query the
            // source buffer for preamble files. For the same reason, we ask
            // shouldSuppressDiagnostic to avoid I/O.
            // We let suppression comments take precedence over warning-as-error
            // to match clang-tidy's behaviour.
            bool in_main_file =
                diag.hasSourceManager() &&
                ast::is_inside_main_file(diag.getLocation(), diag.getSourceManager());
            llvm::SmallVector<clang::tooling::Diagnostic, 1> tidy_suppressed_errors;
            if(in_main_file && context.shouldSuppressDiagnostic(level,
                                                                diag,
                                                                tidy_suppressed_errors,
                                                                /*AllowIO=*/false,
                                                                /*EnableNolintBlocks=*/true)) {
                // FIXME: should we expose the suppression error (invalid use of
                // NOLINT comments)?
                return clang::DiagnosticsEngine::Ignored;
            }
            if(!context.getOptions().SystemHeaders.value_or(false) && diag.hasSourceManager() &&
               diag.getSourceManager().isInSystemMacro(diag.getLocation())) {
                return clang::DiagnosticsEngine::Ignored;
            }

            // Check for warning-as-error.
            if(level == clang::DiagnosticsEngine::Warning && context.treatAsError(tidy_diag)) {
                return clang::DiagnosticsEngine::Error;
            }
        }
    }
    return level;
}

void ClangTidyChecker::adjust_diag(Diagnostic& diag) {
    std::string tidy_diag = context.getCheckName(diag.id.value);
    if(!tidy_diag.empty()) {
        // TODO: using a global string saver.
        static llvm::BumpPtrAllocator allocator;
        static llvm::StringSaver saver(allocator);
        diag.id.name = saver.save(tidy_diag);
        diag.id.source = DiagnosticSource::ClangTidy;
        // clang-tidy bakes the name into diagnostic messages. Strip it out.
        // It would be much nicer to make clang-tidy not do this.
        auto clean_message = [&](std::string& msg) {
            llvm::StringRef rest(msg);
            if(rest.consume_back("]") && rest.consume_back(diag.id.name) && rest.consume_back(" ["))
                msg.resize(rest.size());
        };
        clean_message(diag.message);
        // todo: where is clice notes and fixes?
        // for(auto& note: diag.Notes)
        //     clean_message(note.Message);
        // for(auto& fix: diag.Fixes)
        //     clean_message(fix.Message);
    }
}

std::unique_ptr<ClangTidyChecker> configure(clang::CompilerInstance& instance,
                                            const TidyParams& params) {
    auto& input = instance.getFrontendOpts().Inputs[0];

    if(!input.isFile()) {
        return nullptr;
    }
    auto file_name = input.getFile();
    LOG_INFO("Tidy configure file: {}", file_name);

    tidy::ClangTidyOptions opts = create_options();
    if(opts.Checks) {
        LOG_INFO("Tidy configure checks: {}", *opts.Checks);
    }

    {
        // If clang-tidy is configured to emit clang warnings, we should too.
        //
        // Such clang-tidy configuration consists of two parts:
        //   - ExtraArgs: ["-Wfoo"] causes clang to produce the warnings
        //   - Checks: "clang-diagnostic-foo" prevents clang-tidy filtering them out
        //
        // In clang-tidy, diagnostics are emitted if they pass both checks.
        // When groups contain subgroups, -Wparent includes the child, but
        // clang-diagnostic-parent does not.
        //
        // We *don't* want to change the compile command directly. This can have
        // too many unexpected effects: breaking the command, interactions with
        // -- and -Werror, etc. Besides, we've already parsed the command.
        // Instead we parse the -W<group> flags and handle them directly.
        //
        // Similarly, we don't want to use Checks to filter clang diagnostics after
        // they are generated, as this spreads clang-tidy emulation everywhere.
        // Instead, we just use these to filter which extra diagnostics we enable.
        auto& diags = instance.getDiagnostics();
        TidyDiagnosticGroups groups(opts.Checks ? *opts.Checks : llvm::StringRef());
        if(opts.ExtraArgsBefore) {
            apply_warning_options(*opts.ExtraArgsBefore, groups, diags);
        }
        if(opts.ExtraArgs) {
            apply_warning_options(*opts.ExtraArgs, groups, diags);
        }
    }

    /// No need to run clang-tidy or IncludeFixerif we are not going to surface
    /// diagnostics.
    const static auto all_factories = [] {
        tidy::ClangTidyCheckFactories factories;
        for(const auto& e: tidy::ClangTidyModuleRegistry::entries()) {
            e.instantiate()->addCheckFactories(factories);
        }
        return factories;
    }();
    tidy::ClangTidyCheckFactories factories = get_fast_checks(all_factories);
    std::unique_ptr<ClangTidyChecker> checker = std::make_unique<ClangTidyChecker>(
        std::make_unique<tidy::DefaultOptionsProvider>(tidy::ClangTidyGlobalOptions(), opts));

    checker->context.setDiagnosticsEngine(
        std::make_unique<clang::DiagnosticOptions>(instance.getDiagnosticOpts()),
        &instance.getDiagnostics());
    checker->context.setASTContext(&instance.getASTContext());
    // TODO: is `file_name` always the file to check?
    checker->context.setCurrentFile(file_name);
    checker->context.setSelfContainedDiags(true);
    checker->checks = factories.createChecksForLanguage(&checker->context);
    LOG_INFO("Tidy configure checks: {}", checker->checks.size());
    clang::Preprocessor* pp = &instance.getPreprocessor();
    for(const auto& check: checker->checks) {
        check->registerPPCallbacks(instance.getSourceManager(), pp, pp);
        check->registerMatchers(&checker->finder);
    }
    return checker;
}

}  // namespace clice::tidy
