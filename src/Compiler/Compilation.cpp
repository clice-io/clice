#include "Compiler/Compilation.h"

#include "Implement.h"
#include "AST/Utility.h"
#include "Compiler/Command.h"
#include "Compiler/Diagnostic.h"
#include "Support/Logging.h"

#include "llvm/Support/Error.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/PreprocessorOptions.h"

namespace clice {

namespace {

/// A wrapper ast consumer, so that we can cancel the ast parse
class ProxyASTConsumer final : public clang::MultiplexConsumer {
public:
    ProxyASTConsumer(std::unique_ptr<clang::ASTConsumer> consumer,
                     clang::CompilerInstance& instance,
                     std::vector<clang::Decl*>* top_level_decls,
                     std::shared_ptr<std::atomic_bool> stop) :
        clang::MultiplexConsumer(std::move(consumer)), instance(instance),
        src_mgr(instance.getSourceManager()), top_level_decls(top_level_decls), stop(stop) {}

    void collect_decl(clang::Decl* decl) {
        if(!(ast::is_inside_main_file(decl->getLocation(), src_mgr))) {
            return;
        }

        if(const clang::NamedDecl* named_decl = dyn_cast<clang::NamedDecl>(decl)) {
            if(ast::is_implicit_template_instantiation(named_decl)) {
                return;
            }
        }

        top_level_decls->push_back(decl);
    }

    auto HandleTopLevelDecl(clang::DeclGroupRef group) -> bool final {
        if(top_level_decls) {
            if(group.isDeclGroup()) {
                for(auto decl: group) {
                    collect_decl(decl);
                }
            } else {
                collect_decl(group.getSingleDecl());
            }
        }

        /// TODO: check atomic variable after the parse of each declaration
        /// may result in performance issue, benchmark in the future.
        if(stop && stop->load()) {
            return false;
        }

        return clang::MultiplexConsumer::HandleTopLevelDecl(group);
    }

private:
    clang::CompilerInstance& instance;
    clang::SourceManager& src_mgr;

    /// Non-nullptr if we need collect the top level declarations.
    std::vector<clang::Decl*>* top_level_decls;

    std::shared_ptr<std::atomic_bool> stop;
};

class ProxyAction final : public clang::WrapperFrontendAction {
public:
    ProxyAction(std::unique_ptr<clang::FrontendAction> action,
                std::vector<clang::Decl*>* top_level_decls,
                std::shared_ptr<std::atomic_bool> stop) :
        clang::WrapperFrontendAction(std::move(action)), top_level_decls(top_level_decls),
        stop(std::move(stop)) {}

    auto CreateASTConsumer(clang::CompilerInstance& instance, llvm::StringRef file)
        -> std::unique_ptr<clang::ASTConsumer> final {
        return std::make_unique<ProxyASTConsumer>(
            WrapperFrontendAction::CreateASTConsumer(instance, file),
            instance,
            top_level_decls,
            std::move(stop));
    }

    /// Make this public.
    using clang::WrapperFrontendAction::EndSourceFile;

private:
    std::vector<clang::Decl*>* top_level_decls;
    std::shared_ptr<std::atomic_bool> stop;
};

/// create a `clang::CompilerInvocation` for compilation, it set and reset
/// all necessary arguments and flags for clice compilation.
auto create_invocation(CompilationUnitRef::Self& self,
                       CompilationParams& params,
                       llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine>& diagnostic_engine)
    -> std::unique_ptr<clang::CompilerInvocation> {
    if(params.arguments.empty()) {
        LOG_ERROR_RET(nullptr, "Fail to create invocation: empty argument list from database");
    }

    std::unique_ptr<clang::CompilerInvocation> invocation;

    /// Arguments from compilation database are already cc1
    if(params.arguments_from_database) {
        invocation = std::make_unique<clang::CompilerInvocation>();
        if(!clang::CompilerInvocation::CreateFromArgs(*invocation,
                                                      llvm::ArrayRef(params.arguments).drop_front(),
                                                      *diagnostic_engine,
                                                      params.arguments[0])) {
            LOG_ERROR_RET(nullptr,
                          " Fail to create invocation, arguments list is: {}",
                          print_argv(params.arguments));
        }
    } else {
        /// Create clang invocation.
        clang::CreateInvocationOptions options = {
            .Diags = diagnostic_engine,
            .VFS = params.vfs,

            /// Avoid replacing -include with -include-pch, also
            /// see https://github.com/clangd/clangd/issues/856.
            .ProbePrecompiled = false,
        };

        invocation = clang::createInvocation(params.arguments, options);
        if(!invocation) {
            LOG_ERROR_RET(nullptr,
                          " Fail to create invocation, arguments list is: {}",
                          print_argv(params.arguments));
        }
    }

    auto& pp_opts = invocation->getPreprocessorOpts();

    // CompilerInstance does not deterministically clear RetainRemappedFileBuffers,
    // especially if compilation aborts early, so we keep them alive and clean up
    // in CompilationUnit's destructor instead.
    pp_opts.RetainRemappedFileBuffers = true;

    for(auto& [file, buffer]: params.buffers) {
        pp_opts.addRemappedFile(file, buffer.get());
    }
    self.remapped_buffers = std::move(params.buffers);

    auto [pch, bound] = params.pch;
    pp_opts.ImplicitPCHInclude = std::move(pch);
    if(bound != 0) {
        pp_opts.PrecompiledPreambleBytes = {bound, false};
    }

    // We don't want to write comment locations into PCM. They are racy and slow
    // to read back. We rely on dynamic index for the comments instead.
    pp_opts.WriteCommentListToPCH = false;

    auto& header_search_opts = invocation->getHeaderSearchOpts();
    header_search_opts.Verbose = false;
    for(auto& [name, path]: params.pcms) {
        header_search_opts.PrebuiltModuleFiles.try_emplace(name.str(), std::move(path));
    }

    auto& front_opts = invocation->getFrontendOpts();
    front_opts.DisableFree = false;
    front_opts.ShowHelp = false;
    front_opts.ShowStats = false;
    front_opts.ShowVersion = false;
    front_opts.StatsFile = "";
    front_opts.TimeTracePath = "";
    front_opts.TimeTraceVerbose = false;
    front_opts.TimeTraceGranularity = false;
    front_opts.PrintSupportedCPUs = false;
    front_opts.PrintEnabledExtensions = false;
    front_opts.PrintSupportedExtensions = false;

    /// Compiler flags (like gcc/clang's -M, -MD, -MMD, -H, or msvc's /showIncludes)
    /// can generate dependency files or print included headers to stdout/stderr.
    ///
    /// This output can interfere with or corrupt the Language Server Protocol (LSP)
    /// communication if the server is configured to use stdio for its JSON-RPC transport.
    /// We explicitly disables all related options to ensure no side-effect output is
    /// generated during parsing.
    auto& deps_opts = invocation->getDependencyOutputOpts();
    deps_opts.IncludeSystemHeaders = false;
    deps_opts.ShowSkippedHeaderIncludes = false;
    deps_opts.UsePhonyTargets = false;
    deps_opts.AddMissingHeaderDeps = false;
    deps_opts.IncludeModuleFiles = false;
    deps_opts.ShowIncludesDest = clang::ShowIncludesDestination::None;
    deps_opts.OutputFile.clear();
    deps_opts.HeaderIncludeOutputFile.clear();
    deps_opts.Targets.clear();
    deps_opts.ExtraDeps.clear();
    deps_opts.DOTOutputFile.clear();
    deps_opts.ModuleDependencyOutputDir.clear();

    auto& lang_opts = invocation->getLangOpts();
    lang_opts.CommentOpts.ParseAllComments = true;
    lang_opts.RetainCommentsFromSystemHeaders = true;

    return invocation;
}

CompilationStatus run_clang(CompilationUnitRef::Self& self,
                            CompilationParams& params,
                            std::unique_ptr<clang::FrontendAction> action,
                            llvm::function_ref<void(clang::CompilerInstance&)> before_execute) {
    std::unique_ptr diagnostic_consumer = create_diagnostic(&self);

    /// Temporary diagnostic engine, only used for command line parsing.
    /// For compilation, we need to create a new diagnostic engine. See also
    /// https://github.com/llvm/llvm-project/pull/139584#issuecomment-2920704282.
    clang::DiagnosticOptions options;
    llvm::IntrusiveRefCntPtr diagnostic_engine =
        clang::CompilerInstance::createDiagnostics(*params.vfs,
                                                   options,
                                                   diagnostic_consumer.get(),
                                                   false);
    if(!diagnostic_engine) {
        return CompilationStatus::SetupFail;
    }

    std::unique_ptr invocation = create_invocation(self, params, diagnostic_engine);
    if(!invocation) {
        return CompilationStatus::SetupFail;
    }

    self.instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    auto& instance = *self.instance;
    instance.createDiagnostics(*params.vfs, diagnostic_consumer.release(), true);

    if(auto remapping = clang::createVFSFromCompilerInvocation(instance.getInvocation(),
                                                               instance.getDiagnostics(),
                                                               params.vfs)) {
        instance.createFileManager(std::move(remapping));
    }

    if(!instance.createTarget()) {
        return CompilationStatus::SetupFail;
    }

    if(before_execute) {
        before_execute(instance);
    }

    self.action = std::make_unique<ProxyAction>(
        std::move(action),
        /// We only collect top level declarations for parse main file.
        (params.clang_tidy || params.kind == CompilationKind::Content) ? &self.top_level_decls
                                                                       : nullptr,
        params.stop);

    if(!self.action->BeginSourceFile(instance, instance.getFrontendOpts().Inputs[0])) {
        self.action.reset();
        return CompilationStatus::SetupFail;
    }

    /// FIXME: include-fixer, etc?

    /// Add PPCallbacks to collect preprocessing information.
    self.collect_directives();

    if(params.clang_tidy) {
        self.configure_tidy({});
    }

    std::optional<clang::syntax::TokenCollector> token_collector;
    if(!instance.hasCodeCompletionConsumer()) {
        /// It is not necessary to collect tokens if we are running code completion.
        /// And in fact will cause assertion failure.
        token_collector.emplace(instance.getPreprocessor());
    }

    if(auto error = self.action->Execute()) {
        // Upstream FrontendAction::Execute() always returns success (errors go through
        // diagnostics); log here only as a guard in case a custom action ever returns
        // an unexpected llvm::Error.
        LOG_ERROR("FrontendAction::Execute failed: {}", error);
        return CompilationStatus::FatalError;
    }

    /// If the output file is not empty, it represents that we are
    /// generating a PCH or PCM. If error occurs, the AST must be
    /// invalid to some extent, serialization of such AST may result
    /// in crash frequently. So forbidden it here and return as error.
    if(!instance.getFrontendOpts().OutputFile.empty() &&
       instance.getDiagnostics().hasErrorOccurred()) {
        return CompilationStatus::FatalError;
    }

    /// Check whether the compilation is canceled, if so we think
    /// it is an error.
    if(params.stop && params.stop->load()) {
        self.action->EndSourceFile();
        self.action.reset();
        return CompilationStatus::Cancelled;
    }

    if(token_collector) {
        self.buffer = std::move(*token_collector).consume();
    }

    self.run_tidy();

    if(instance.hasSema()) {
        self.resolver.emplace(instance.getSema());
    }

    return CompilationStatus::Completed;
}

CompilationUnit run_clang(CompilationParams& params,
                          std::unique_ptr<clang::FrontendAction> action,
                          llvm::function_ref<void(clang::CompilerInstance&)> before_execute = {},
                          llvm::function_ref<void(CompilationUnitRef)> after_execute = {}) {
    auto self = new CompilationUnitRef::Self();
    self->kind = params.kind;

    using namespace std::chrono;
    self->build_at = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    auto build_start = steady_clock::now().time_since_epoch();

    self->status = run_clang(*self, params, std::move(action), before_execute);

    auto build_end = steady_clock::now().time_since_epoch();
    self->build_duration = duration_cast<milliseconds>(build_end - build_start);

    if(self->status == CompilationStatus::Completed && after_execute) {
        after_execute(self);
    }

    return CompilationUnit(self);
}

}  // namespace

CompilationUnit preprocess(CompilationParams& params) {
    return run_clang(params, std::make_unique<clang::PreprocessOnlyAction>());
}

CompilationUnit compile(CompilationParams& params) {
    return run_clang(params,
                     std::make_unique<clang::SyntaxOnlyAction>(),
                     [](clang::CompilerInstance& instance) {
                         /// Make sure the output file is empty.
                         instance.getFrontendOpts().OutputFile.clear();
                     });
}

CompilationUnit compile(CompilationParams& params, PCHInfo& out) {
    assert(!params.output_file.empty() && "PCH file path cannot be empty");

    /// Record the begin time of PCH building.
    auto now = std::chrono::system_clock::now().time_since_epoch();
    out.mtime = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    return run_clang(
        params,
        std::make_unique<clang::GeneratePCHAction>(),
        [&](clang::CompilerInstance& instance) {
            /// Set options to generate PCH.
            instance.getFrontendOpts().OutputFile = params.output_file.str();
            instance.getFrontendOpts().ProgramAction = clang::frontend::GeneratePCH;
            instance.getPreprocessorOpts().GeneratePreamble = true;

            // We don't want to write comment locations into PCH. They are racy and slow
            // to read back. We rely on dynamic index for the comments instead.
            instance.getPreprocessorOpts().WriteCommentListToPCH = false;

            instance.getLangOpts().CompilingPCH = true;
        },
        [&](CompilationUnitRef unit) {
            out.path = params.output_file.str();
            out.preamble = unit.interested_content();
            out.deps = unit.deps();
            out.arguments = params.arguments;
        });
}

CompilationUnit compile(CompilationParams& params, PCMInfo& out) {
    assert(!params.output_file.empty() && "PCM file path cannot be empty");

    return run_clang(
        params,
        std::make_unique<clang::GenerateReducedModuleInterfaceAction>(),
        [&](clang::CompilerInstance& instance) {
            /// Set options to generate PCH.
            instance.getFrontendOpts().OutputFile = params.output_file.str();
            instance.getFrontendOpts().ProgramAction =
                clang::frontend::GenerateReducedModuleInterface;

            out.srcPath = instance.getFrontendOpts().Inputs[0].getFile();
        },
        [&](CompilationUnitRef unit) {
            out.path = params.output_file.str();

            for(auto& [name, path]: params.pcms) {
                out.mods.emplace_back(name);
            }
        });
}

CompilationUnit complete(CompilationParams& params, clang::CodeCompleteConsumer* consumer) {
    auto& [file, offset] = params.completion;

    /// The location of clang is 1-1 based.
    std::uint32_t line = 1;
    std::uint32_t column = 1;

    /// FIXME:
    assert(params.buffers.size() == 1);
    llvm::StringRef content = params.buffers.begin()->second->getBuffer();

    for(auto c: content.substr(0, offset)) {
        if(c == '\n') {
            line += 1;
            column = 1;
            continue;
        }
        column += 1;
    }

    return run_clang(params,
                     std::make_unique<clang::SyntaxOnlyAction>(),
                     [&](clang::CompilerInstance& instance) {
                         /// Set options to run code completion.
                         instance.getFrontendOpts().CodeCompletionAt.FileName = std::move(file);
                         instance.getFrontendOpts().CodeCompletionAt.Line = line;
                         instance.getFrontendOpts().CodeCompletionAt.Column = column;
                         instance.setCodeCompletionConsumer(consumer);
                     });
}

}  // namespace clice
