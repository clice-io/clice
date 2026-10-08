module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

#include "clang-tidy/ClangTidyForceLinker.h"

module clice;

import :compile.implement;
import :support.logging;
import :vfs.file_system;

namespace clice::tidy {

namespace {

bool is_inside_main_file(clang::SourceLocation loc, const clang::SourceManager& sm) {
    if(!loc.isValid()) {
        return false;
    }
    clang::FileID fid = sm.getFileID(sm.getExpansionLoc(loc));
    return fid == sm.getMainFileID() || fid == sm.getPreambleFileID();
}

/// The #include directives of the main file's preamble, replayed to the
/// tidy checks' callbacks: a compile against a PCH never preprocesses
/// them. Include-aware checks look for them, and a fix inserting an
/// include places it among them, or leaves out one the file already has.
/// clangd's ReplayPreamble, read from the source: a directive whose file
/// the PCH never included sat in an inactive branch, and one naming its
/// file through a macro has no filename to replay.
class PreambleReplay : public clang::PPCallbacks {
public:
    PreambleReplay(clang::Preprocessor& pp, clang::PPCallbacks& delegate, unsigned bound) :
        pp(pp), delegate(delegate), bound(bound) {}

    /// The preamble's includes follow the predefines buffer, the last
    /// thing the compile enters before the main file's rest.
    void FileChanged(clang::SourceLocation,
                     FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind,
                     clang::FileID prev) override {
        if(reason == ExitFile &&
           pp.getSourceManager().getBufferOrFake(prev).getBufferIdentifier() == "<built-in>") {
            replay();
        }
    }

private:
    void replay() {
        auto& sm = pp.getSourceManager();
        auto main = sm.getMainFileID();
        // Lexed over the whole buffer, whose terminating null the lexer
        // relies on, up to the bound.
        auto text = sm.getBufferData(main);
        clang::Lexer lexer(sm.getLocForStartOfFile(main),
                           pp.getLangOpts(),
                           text.begin(),
                           text.begin(),
                           text.end());
        while(true) {
            clang::Token hash;
            lexer.LexFromRawLexer(hash);
            if(sm.getFileOffset(hash.getLocation()) >= bound) {
                return;
            }
            if(hash.isNot(clang::tok::hash) || !hash.isAtStartOfLine()) {
                continue;
            }
            clang::Token keyword;
            lexer.LexFromRawLexer(keyword);
            if(keyword.is(clang::tok::raw_identifier) &&
               llvm::is_contained({"include", "include_next", "import"},
                                  keyword.getRawIdentifier())) {
                replay_include(hash, keyword, text);
            }
        }
    }

    void replay_include(const clang::Token& hash, clang::Token& keyword, llvm::StringRef text) {
        auto& sm = pp.getSourceManager();
        auto line = text.drop_front(sm.getFileOffset(keyword.getEndLoc()))
                        .ltrim(" \t")
                        .take_until([](char c) { return c == '\n' || c == '\r'; });
        if(line.empty() || (line.front() != '<' && line.front() != '"')) {
            return;
        }
        auto close = line.find(line.front() == '<' ? '>' : '"', 1);
        if(close == llvm::StringRef::npos) {
            return;
        }
        auto written = line.take_front(close + 1);
        auto name = written.drop_front().drop_back();
        bool angled = written.front() == '<';
        auto location = sm.getLocForStartOfFile(sm.getMainFileID())
                            .getLocWithOffset(written.data() - text.data());
        auto file = pp.LookupFile(location,
                                  name,
                                  angled,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr);
        if(!file || !pp.alreadyIncluded(*file)) {
            return;
        }

        pp.LookUpIdentifierInfo(keyword);
        clang::Token filename;
        filename.startToken();
        filename.setKind(clang::tok::header_name);
        filename.setLocation(location);
        filename.setLength(written.size());
        filename.setLiteralData(written.data());
        auto kind = pp.getHeaderSearchInfo().getFileDirFlavor(*file);
        delegate.InclusionDirective(
            hash.getLocation(),
            keyword,
            name,
            angled,
            clang::CharSourceRange::getCharRange(location, filename.getEndLoc()),
            file,
            /*SearchPath=*/"",
            /*RelativePath=*/"",
            /*SuggestedModule=*/nullptr,
            /*ModuleImported=*/false,
            kind);
        delegate.FileSkipped(*file, filename, kind);
    }

    clang::Preprocessor& pp;
    clang::PPCallbacks& delegate;
    unsigned bound;
};

/// The view of the disk a .clang-tidy resolution reads, recording each
/// configuration file it looks for: by the hash of the text it read, or
/// as absent.
class ConfigReads : public llvm::vfs::ProxyFileSystem {
public:
    ConfigReads() : ProxyFileSystem(llvm::makeIntrusiveRefCnt<vfs::View>()) {}

    std::vector<DepFile> files;

    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override {
        auto status = ProxyFileSystem::status(path);
        if(!status && is_config(path)) {
            files.push_back({.path = path.str(), .absent = true});
        }
        return status;
    }

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
        openFileForRead(const llvm::Twine& path) override {
        auto file = ProxyFileSystem::openFileForRead(path);
        if(!file || !is_config(path)) {
            return file;
        }
        auto status = (*file)->status();
        auto buffer = (*file)->getBuffer(path);
        if(!status || !buffer) {
            return status ? buffer.getError() : status.getError();
        }
        files.push_back({.path = path.str(), .hash = llvm::xxh3_64bits((*buffer)->getBuffer())});
        return std::make_unique<ReadFile>(*status, std::move(*buffer));
    }

private:
    /// The text a recorded read hands on, so the provider parses the bytes
    /// the record hashed.
    class ReadFile : public llvm::vfs::File {
    public:
        ReadFile(llvm::vfs::Status status, std::unique_ptr<llvm::MemoryBuffer> buffer) :
            stat(std::move(status)), buffer(std::move(buffer)) {}

        llvm::ErrorOr<llvm::vfs::Status> status() override {
            return stat;
        }

        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
            getBuffer(const llvm::Twine& name, std::int64_t, bool, bool) override {
            return llvm::MemoryBuffer::getMemBufferCopy(buffer->getBuffer(), name);
        }

        std::error_code close() override {
            return {};
        }

    private:
        llvm::vfs::Status stat;
        std::unique_ptr<llvm::MemoryBuffer> buffer;
    };

    static bool is_config(const llvm::Twine& path) {
        llvm::SmallString<256> storage;
        return llvm::sys::path::filename(path.toStringRef(storage)) == ".clang-tidy";
    }
};

}  // namespace

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
#include "compile/tidy_fast_checks.inc"
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

tidy::ClangTidyOptions create_options(const TidyParams& params) {
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

                         // Check relies on seeing ifndef/define/endif directives,
                         // clangd doesn't replay those when using a preamble.
                         "-llvm-header-guard",
                         "-modernize-macro-to-enum",
                         "-cppcoreguidelines-macro-to-enum",

                         // Check can choke on invalid (intermediate) c++
                         // code, which is often the case when clangd
                         // tries to build an AST.
                         "-bugprone-use-after-move",
                         // Check uses dataflow analysis, which might hang/crash unexpectedly on
                         // incomplete code.
                         "-bugprone-unchecked-optional-access",
                         "-abseil-unchecked-statusor-access");

    tidy::ClangTidyOptions opts = default_opts;

    // clang::clangd::provideEnvironment
    if(std::optional<std::string> user = llvm::sys::Process::GetEnv("USER")) {
        opts.User = user;
    }
    // clang::clangd::provideDefaultChecks
    opts.Checks = params.checks.empty() ? default_checks : params.checks;
    // clang::clangd::disableUnusableChecks, after the configuration's own
    // list so that no configuration enables them again. A batch run never
    // parses code mid-edit.
    if(!params.batch) {
        opts.Checks->append(bad_checks);
    }
    for(auto& [key, value]: params.options) {
        opts.CheckOptions.insert_or_assign(key, tidy::ClangTidyOptions::ClangTidyValue(value));
    }
    if(!params.warnings_as_errors.empty()) {
        opts.WarningsAsErrors = params.warnings_as_errors;
    }
    if(!params.header_filter.empty()) {
        opts.HeaderFilterRegex = params.header_filter;
    }
    if(!params.exclude_header_filter.empty()) {
        opts.ExcludeHeaderFilterRegex = params.exclude_header_filter;
    }
    if(params.system_headers) {
        opts.SystemHeaders = true;
    }
    if(!params.header_file_extensions.empty()) {
        opts.HeaderFileExtensions = params.header_file_extensions;
    }
    if(!params.implementation_file_extensions.empty()) {
        opts.ImplementationFileExtensions = params.implementation_file_extensions;
    }
    if(!params.extra_args.empty()) {
        opts.ExtraArgs = params.extra_args;
    }
    if(!params.extra_args_before.empty()) {
        opts.ExtraArgsBefore = params.extra_args_before;
    }
    return opts;
}

CommandExtraArgs command_extra_args(llvm::ArrayRef<std::string> extra_args,
                                    llvm::ArrayRef<std::string> extra_args_before) {
    // -Wp,/-Wl,/-Wa, are driver pass-throughs, not warning flags: they
    // must reach the command, while true -W warning flags stay on the
    // warning-options path where the Checks gate applies.
    auto non_warning = [](llvm::StringRef ref) {
        return !ref.starts_with("-W") || ref.starts_with("-Wp,") || ref.starts_with("-Wl,") ||
               ref.starts_with("-Wa,");
    };
    // A -X<tool> forwards its next token, so the pair filters as one
    // unit on the operand's verdict — a lone survivor would consume
    // whatever follows it on the final command (the source path, say).
    auto forwards_operand = [](llvm::StringRef ref) {
        return ref == "-Xclang" || ref == "-Xpreprocessor" || ref == "-Xlinker" ||
               ref == "-Xassembler" || ref.starts_with("-Xarch_");
    };
    auto filter_into = [&](llvm::ArrayRef<std::string> args, std::vector<std::string>& out) {
        for(std::size_t i = 0; i < args.size(); i += 1) {
            llvm::StringRef ref(args[i]);
            if(forwards_operand(ref) && i + 1 < args.size()) {
                if(non_warning(args[i + 1])) {
                    out.push_back(args[i]);
                    out.push_back(args[i + 1]);
                }
                i += 1;
            } else if(non_warning(ref)) {
                out.push_back(args[i]);
            }
        }
    };
    CommandExtraArgs split;
    filter_into(extra_args_before, split.prepend);
    filter_into(extra_args, split.append);
    return split;
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

ClangTidyChecker::ClangTidyChecker(std::unique_ptr<ClangTidyOptionsProvider> provider,
                                   clang::ast_matchers::MatchFinder::MatchFinderOptions options) :
    context(std::move(provider)), finder(options) {}

clang::DiagnosticsEngine::Level
    ClangTidyChecker::adjust_level(clang::DiagnosticsEngine::Level level,
                                   const clang::Diagnostic& diag) {
    if(checks.empty()) {
        return level;
    }
    // Compiler warnings carry their clang-diagnostic-* name too. A batch
    // run neither suppresses nor promotes the ones Checks leaves out: they
    // are no findings of its, as in clang-tidy.
    std::string tidy_diag = context.getCheckName(diag.getID());
    if(tidy_diag.empty() || (batch && context.isCompilerDiagnostic(diag.getID()) &&
                             !context.isCheckEnabled(tidy_diag))) {
        return level;
    }
    // Check for suppression comment. The interactive shape skips
    // diagnostics outside the main file and forbids I/O: that
    // function would otherwise read the source buffers of preamble
    // files. The batch shape reads every file, as clang-tidy does.
    // We let suppression comments take precedence over warning-as-error
    // to match clang-tidy's behaviour.
    bool in_main_file =
        diag.hasSourceManager() && is_inside_main_file(diag.getLocation(), diag.getSourceManager());
    llvm::SmallVector<clang::tooling::Diagnostic, 1> tidy_suppressed_errors;
    if((in_main_file || batch) && context.shouldSuppressDiagnostic(level,
                                                                   diag,
                                                                   tidy_suppressed_errors,
                                                                   /*AllowIO=*/batch,
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
    return level;
}

void ClangTidyChecker::adjust_diag(Diagnostic& diag) {
    std::string tidy_diag = context.getCheckName(diag.id.value);
    // A compiler diagnostic stays the compiler's, but for a batch run's
    // finding.
    if(tidy_diag.empty() || (context.isCompilerDiagnostic(diag.id.value) &&
                             !(batch && context.isCheckEnabled(tidy_diag)))) {
        return;
    }
    diag.id.name = names.save(tidy_diag);
    diag.id.source = DiagnosticSource::ClangTidy;
    // clang-tidy bakes the name into diagnostic messages. Strip it out.
    // It would be much nicer to make clang-tidy not do this.
    llvm::StringRef rest(diag.message);
    if(rest.consume_back("]") && rest.consume_back(diag.id.name) && rest.consume_back(" [")) {
        diag.message.resize(rest.size());
    }
}

std::unique_ptr<ClangTidyChecker> configure(clang::CompilerInstance& instance,
                                            const TidyParams& params) {
    auto& input = instance.getFrontendOpts().Inputs[0];

    if(!input.isFile()) {
        return nullptr;
    }
    auto file_name = input.getFile();
    tidy::ClangTidyOptions opts = create_options(params);

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
    tidy::ClangTidyCheckFactories factories =
        params.batch ? all_factories : get_fast_checks(all_factories);
    // Like clang-tidy: nodes in system headers are not even matched unless
    // the configuration asks for their findings.
    clang::ast_matchers::MatchFinder::MatchFinderOptions finder_options;
    finder_options.IgnoreSystemHeaders = !opts.SystemHeaders.value_or(false);
    std::unique_ptr<ClangTidyChecker> checker = std::make_unique<ClangTidyChecker>(
        std::make_unique<tidy::DefaultOptionsProvider>(tidy::ClangTidyGlobalOptions(), opts),
        finder_options);
    checker->batch = params.batch;

    checker->context.setDiagnosticsEngine(
        std::make_unique<clang::DiagnosticOptions>(instance.getDiagnosticOpts()),
        &instance.getDiagnostics());
    checker->context.setASTContext(&instance.getASTContext());
    // TODO: is `file_name` always the file to check?
    checker->context.setCurrentFile(file_name);
    checker->context.setSelfContainedDiags(true);
    checker->checks = factories.createChecksForLanguage(&checker->context);
    LOG_DEBUG("Tidy configured for {}: {} checks of {}",
              file_name,
              checker->checks.size(),
              *opts.Checks);
    clang::Preprocessor* pp = &instance.getPreprocessor();
    for(const auto& check: checker->checks) {
        check->registerPPCallbacks(instance.getSourceManager(), pp, pp);
        check->registerMatchers(&checker->finder);
    }
    // The checks' callbacks are all the preprocessor has yet: the replay
    // reaches them alone, never the unit's own collectors added later.
    if(auto bound = instance.getPreprocessorOpts().PrecompiledPreambleBytes.first;
       bound != 0 && pp->getPPCallbacks()) {
        pp->addPPCallbacks(std::make_unique<PreambleReplay>(*pp, *pp->getPPCallbacks(), bound));
    }
    return checker;
}

TidyResolution resolve_tidy_params(llvm::StringRef file) {
    // clang-tidy's own provider: walks the file's ancestor directories
    // reading .clang-tidy, honoring InheritParentConfig. The defaults
    // carry no checks, so a tree without any configuration resolves to an
    // empty list and the consumer's built-in default set applies.
    auto reads = llvm::makeIntrusiveRefCnt<ConfigReads>();
    tidy::FileOptionsProvider provider(
        tidy::ClangTidyGlobalOptions(),
        [] {
            auto opts = tidy::ClangTidyOptions::getDefaults();
            opts.Checks->clear();
            return opts;
        }(),
        tidy::ClangTidyOptions(),
        reads);
    auto opts = provider.getOptions(file);

    TidyResolution resolution;
    auto& params = resolution.params;
    params.checks = opts.Checks.value_or(std::string());
    for(auto& [key, value]: opts.CheckOptions) {
        params.options.emplace_back(key.str(), value.Value);
    }
    // Deterministic plan bytes: the option map's iteration order is not.
    std::ranges::sort(params.options);
    params.warnings_as_errors = opts.WarningsAsErrors.value_or(std::string());
    params.header_filter = opts.HeaderFilterRegex.value_or(std::string());
    params.exclude_header_filter = opts.ExcludeHeaderFilterRegex.value_or(std::string());
    params.system_headers = opts.SystemHeaders.value_or(false);
    params.header_file_extensions = opts.HeaderFileExtensions.value_or(std::vector<std::string>());
    params.implementation_file_extensions =
        opts.ImplementationFileExtensions.value_or(std::vector<std::string>());
    params.extra_args = opts.ExtraArgs.value_or(std::vector<std::string>());
    params.extra_args_before = opts.ExtraArgsBefore.value_or(std::vector<std::string>());
    resolution.files = std::move(reads->files);
    return resolution;
}

}  // namespace clice::tidy
