module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.command;
import :command.invocation;
import :compile.compilation;
import :compile.diagnostic;
import :compile.implement;
import :compile.tokens;
import :semantic.decls;
import :support.logging;

namespace clice {

CompilationUnitRef::Self::~Self() {
    if(action) {
        // We already notified the pp of end-of-file earlier, so detach it first.
        // We must keep it alive until after EndSourceFile(), Sema relies on this.
        std::shared_ptr<clang::Preprocessor> pp = instance->getPreprocessorPtr();
        // Detach so we don't send EOF again
        instance->setPreprocessor(nullptr);
        action->EndSourceFile();
    }
}

std::unique_ptr<clang::CompilerInvocation>
    CompilationUnitRef::Self::create_invocation(this Self& self,
                                                CompilationParams& params,
                                                clang::DiagnosticConsumer* consumer) {
    if(params.arguments.empty()) {
        LOG_ERROR_RET(nullptr, "Fail to create invocation: empty argument list from database");
    }

    /// Temporary diagnostic engine, only used for command line parsing.
    /// For compilation, we need to create a new diagnostic engine. See also
    /// https://github.com/llvm/llvm-project/pull/139584#issuecomment-2920704282.
    clang::DiagnosticOptions options;
    llvm::IntrusiveRefCntPtr diagnostic_engine =
        clang::CompilerInstance::createDiagnostics(*params.vfs, options, consumer, false);
    if(!diagnostic_engine) {
        LOG_ERROR_RET(nullptr, "Fail to create diagnostics engine");
    }

    auto invocation = create_compiler_invocation(params.arguments,
                                                 params.directory,
                                                 params.vfs,
                                                 diagnostic_engine);
    if(!invocation) {
        LOG_ERROR_RET(nullptr,
                      " Fail to create invocation, arguments list is: {}",
                      print_argv(params.arguments));
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
    self.synthesized = std::move(params.synthesized);
    if(params.kind != CompilationKind::Preamble) {
        llvm::append_range(pp_opts.Includes, params.forced_includes);
    }

    auto [pch, bound] = params.pch;
    pp_opts.ImplicitPCHInclude = std::move(pch);
    if(bound != 0) {
        pp_opts.PrecompiledPreambleBytes = {bound, false};
    }
    // A preamble with errors still gets its PCH, and every parse loading
    // it accepts it: without one, every edit parses the whole preamble
    // again.
    pp_opts.AllowPCHWithCompilerErrors = true;

    // `#pragma clang __debug crash` and its kin crash the compiler on
    // purpose. Tests keep them as a crash a file's content decides.
    const static bool pragma_crash =
        llvm::sys::Process::GetEnv("CLICE_TEST_PRAGMA_CRASH").has_value();
    pp_opts.DisablePragmaDebugCrash = !pragma_crash;

    // We don't want to write comment locations into PCM. They are racy and slow
    // to read back. We rely on dynamic index for the comments instead.
    pp_opts.WriteCommentListToPCH = false;

    auto& header_search_opts = invocation->getHeaderSearchOpts();
    header_search_opts.Verbose = false;
    for(auto& [name, path]: params.pcms) {
        header_search_opts.PrebuiltModuleFiles.try_emplace(name.str(), std::move(path));
    }

    auto& front_opts = invocation->getFrontendOpts();
    front_opts.ShowHelp = false;
    front_opts.ShowStats = false;
    front_opts.ShowVersion = false;
    front_opts.StatsFile = "";
    front_opts.TimeTracePath = "";
    front_opts.TimeTraceVerbose = false;
    front_opts.TimeTraceGranularity = 0;
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

    // MSVC targets defer template bodies by default so SDK headers written
    // for MSVC's lookup rules still parse, but a deferred body has no AST and
    // every feature inside it goes blind. Only artifact builds keep the
    // default: an error there discards the PCH/PCM, so those headers must
    // keep parsing the way clang meant them to.
    if(params.output_file.empty()) {
        lang_opts.DelayedTemplateParsing = false;
    }

    // A header compiled under a source's command (`-x c++` buys a parse
    // instead of a precompiled-header job) is still a header: no "#pragma
    // once in main file", no unused warnings for its static functions.
    if(is_header_path(front_opts.Inputs[0].getFile())) {
        lang_opts.IsHeaderFile = true;
    }

    return invocation;
}

void CompilationUnitRef::Self::run_tidy() {
    if(!checker) {
        return;
    }
    // The editor matches the main file's declarations alone: the
    // preamble's would cost a walk over every header. The unit's later
    // readers see the whole TU again, and the parent map built over the
    // restricted scope goes with it.
    auto& context = instance->getASTContext();
    if(!checker->batch) {
        context.setTraversalScope(top_level_decls);
    }
    // Checks re-lex through the preprocessor (modernize-use-trailing-return-type
    // does): what the directive collector records meanwhile is no directive
    // of the file.
    auto collected = std::move(directives);

    // Tests crash the pass over a main file holding CLICE_TEST_TIDY_CRASH.
    const static auto crash = llvm::sys::Process::GetEnv("CLICE_TEST_TIDY_CRASH");
    if(crash && SM().getBufferData(SM().getMainFileID()).contains(*crash)) {
        LLVM_BUILTIN_TRAP;
    }

    checker->finder.matchAST(context);

    /// XXX: This is messy: clang-tidy checks flush some diagnostics at EOF.
    /// However Action->EndSourceFile() would destroy the ASTContext!
    /// So just inform the preprocessor of EOF, while keeping everything alive.
    instance->getPreprocessor().EndSourceFile();
    directives = std::move(collected);
    context.setTraversalScope({context.getTranslationUnitDecl()});
}

namespace {

/// A wrapper ast consumer, so that we can cancel the ast parse
class ProxyASTConsumer final : public clang::MultiplexConsumer {
public:
    ProxyASTConsumer(std::unique_ptr<clang::ASTConsumer> consumer, CompilationUnitRef unit) :
        clang::MultiplexConsumer(std::move(consumer)), unit(unit) {}

    void collect_decl(clang::Decl* decl) {
        if(unit.file_id(unit.expansion_location(decl->getLocation())) != unit.main_file() &&
           !unit.encloses_main_file(decl)) {
            return;
        }

        if(const clang::NamedDecl* named_decl = dyn_cast<clang::NamedDecl>(decl)) {
            if(decls::is_implicit_instantiation(named_decl)) {
                return;
            }
        }

        // Sema hands a function template specialization over again for every
        // explicit instantiation directive that finds it already defined.
        if(!collected.insert(decl).second) {
            return;
        }

        // A namespace-scope anonymous union reaches the consumer only as
        // its implicit variable; the written union is the record behind it.
        if(auto* var = llvm::dyn_cast<clang::VarDecl>(decl); var && var->isImplicit()) {
            if(auto* record = var->getType()->getAsRecordDecl();
               record && record->isAnonymousStructOrUnion()) {
                unit->top_level_decls.push_back(record);
            }
        }

        unit->top_level_decls.push_back(decl);
    }

    auto HandleTopLevelDecl(clang::DeclGroupRef group) -> bool final {
        if(unit->kind == CompilationKind::Content) {
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
        if(unit->stop && unit->stop->load()) {
            return false;
        }

        return clang::MultiplexConsumer::HandleTopLevelDecl(group);
    }

    // Sema adds an explicit instantiation directive's decl to its declaration
    // context and never hands it to the consumer, so the ones at file scope
    // are picked up here (a namespace block brings its own along).
    // noload_decls leaves a preamble's declarations on disk.
    void HandleTranslationUnit(clang::ASTContext& context) final {
        if(unit->kind == CompilationKind::Content) {
            for(auto* decl: context.getTranslationUnitDecl()->noload_decls()) {
                if(llvm::isa<clang::ExplicitInstantiationDecl>(decl)) {
                    collect_decl(decl);
                }
            }
        }
        // The PCH writer stores each import with the location where the
        // preprocessor made its module visible, and the reader skips the
        // imports without one; a C++20 import is made visible by Sema alone.
        if(unit->kind == CompilationKind::Preamble) {
            auto& pp = unit->instance->getPreprocessor();
            for(auto* import: context.local_imports()) {
                pp.makeModuleVisible(import->getImportedModule(), import->getLocation());
            }
        }
        clang::MultiplexConsumer::HandleTranslationUnit(context);
    }

private:
    CompilationUnitRef unit;
    llvm::SmallPtrSet<const clang::Decl*, 8> collected;
};

class ProxyAction final : public clang::WrapperFrontendAction {
public:
    ProxyAction(std::unique_ptr<clang::FrontendAction> action, CompilationUnitRef unit) :
        clang::WrapperFrontendAction(std::move(action)), unit(unit) {}

    auto CreateASTConsumer(clang::CompilerInstance& instance, llvm::StringRef file)
        -> std::unique_ptr<clang::ASTConsumer> final {
        auto consumer = WrapperFrontendAction::CreateASTConsumer(instance, file);
        if(!consumer)
            return nullptr;
        return std::make_unique<ProxyASTConsumer>(std::move(consumer), unit);
    }

    /// Make this public.
    using clang::WrapperFrontendAction::EndSourceFile;

private:
    CompilationUnitRef unit;
};

}  // namespace

CompilationStatus CompilationUnitRef::Self::run_clang(
    this Self& self,
    CompilationParams& params,
    std::unique_ptr<clang::FrontendAction> action,
    llvm::function_ref<void(clang::CompilerInstance&)> before_execute) {
    std::unique_ptr diagnostic_consumer = self.create_diagnostic();
    std::unique_ptr invocation = self.create_invocation(params, diagnostic_consumer.get());
    if(!invocation) {
        return CompilationStatus::SetupFail;
    }

    self.instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    auto& instance = *self.instance;
    instance.createVirtualFileSystem(params.vfs, diagnostic_consumer.get());
    instance.createDiagnostics(diagnostic_consumer.release(), true);
    instance.createFileManager();

    if(!instance.createTarget()) {
        return CompilationStatus::SetupFail;
    }

    if(before_execute) {
        before_execute(instance);
    }

    self.action = std::make_unique<ProxyAction>(std::move(action), &self);

    if(!self.action->BeginSourceFile(instance, instance.getFrontendOpts().Inputs[0])) {
        /// If the action is not empty, we will call `EndSourceFile` at the destructor of `Self`.
        /// But if we fail to `BeginSourceFile` we don't need to call `EndSourceFile`. So just
        /// reset it.
        self.action.reset();
        return CompilationStatus::SetupFail;
    }

    /// FIXME: include-fixer, etc?

    /// The checks' PPCallbacks go first: the preamble replay reaches the
    /// callbacks registered before it (see tidy::configure).
    if(params.tidy) {
        self.checker = tidy::configure(instance, *params.tidy, params.preamble_inactive_regions);
    }

    /// Add PPCallbacks to collect preprocessing information.
    self.collect_directives();

    if(params.kind == CompilationKind::Content) {
        self.tokens.emplace(instance.getPreprocessor());
    }

    if(auto error = self.action->Execute()) {
        // Upstream FrontendAction::Execute() always returns success (errors go through
        // diagnostics); log here only as a guard in case a custom action ever returns
        // an unexpected llvm::Error.
        LOG_ERROR("FrontendAction::Execute failed: {}", error);
        return CompilationStatus::FatalError;
    }

    /// If the output file is not empty, it represents that we are
    /// generating a PCH or PCM. A PCM build with errors fails. A PCH is
    /// written in spite of its preamble's errors; clang writes none only
    /// after a module failed to load.
    if(!instance.getFrontendOpts().OutputFile.empty() &&
       (self.kind == CompilationKind::Preamble ? instance.hadModuleLoaderFatalFailure()
                                               : instance.getDiagnostics().hasErrorOccurred())) {
        return CompilationStatus::FatalError;
    }

    /// Check whether the compilation is canceled, if so we think
    /// it is an error.
    if(self.stop && self.stop->load()) {
        return CompilationStatus::Cancelled;
    }

    if(self.tokens) {
        self.tokens->finish();
    }

    self.run_tidy();

    if(instance.hasASTContext()) {
        self.resolver.emplace(instance.getASTContext());
    }

    return CompilationStatus::Completed;
}

CompilationUnit run_clang(CompilationParams& params,
                          std::unique_ptr<clang::FrontendAction> action,
                          llvm::function_ref<void(clang::CompilerInstance&)> before_execute = {},
                          llvm::function_ref<void(CompilationUnitRef)> after_execute = {}) {
    LOG_DEBUG("Compile command: {}", print_argv(params.arguments));

    // Record the stack bottom so that clang's own recursion guards
    // (`clang::runWithSufficientStackSpace` in the parser, constant
    // evaluator, etc.) actually work. They are inert without it and
    // deeply nested code would overflow the stack during parsing.
    clang::noteBottomOfStack();

    auto self = new CompilationUnitRef::Self();
    self->kind = params.kind;
    self->stop = std::move(params.stop);
    self->workspace = std::move(params.workspace);

    using namespace std::chrono;
    self->build_at = duration_cast<milliseconds>(system_clock::now().time_since_epoch());

    self->status = self->run_clang(params, std::move(action), before_execute);

    if(self->status == CompilationStatus::Completed && after_execute) {
        after_execute(self);
    }

    return CompilationUnit(self);
}

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

    auto unit = run_clang(
        params,
        std::make_unique<clang::GeneratePCHAction>(),
        [&](clang::CompilerInstance& instance) {
            /// Set options to generate PCH.
            instance.getFrontendOpts().OutputFile = params.output_file.str();
            instance.getFrontendOpts().ProgramAction = clang::frontend::GeneratePCH;
            instance.getPreprocessorOpts().GeneratePreamble = true;

            // Without recorded mtimes clang checks each input by its size
            // alone. Freshness is the master's call, made on content: a
            // same-bytes rewrite (`git stash pop`, a branch switch) moves
            // only the mtime and must not get a PCH the master still
            // vouches for rejected. The size check stays, as it guards the
            // reader against offsets past the end of a shrunk file.
            instance.getFrontendOpts().IncludeTimestamps = false;

            // We don't want to write comment locations into PCH. They are racy and slow
            // to read back. We rely on dynamic index for the comments instead.
            instance.getPreprocessorOpts().WriteCommentListToPCH = false;

            instance.getLangOpts().CompilingPCH = true;
        },
        [&](CompilationUnitRef unit) {
            out.path = params.output_file.str();
            out.preamble = unit.main_content();
            out.arguments = params.arguments;
        });
    if(unit.completed() || unit.fatal_error()) {
        out.deps = unit.deps();
    }
    return unit;
}

CompilationUnit compile(CompilationParams& params, PCMInfo& out) {
    assert(!params.output_file.empty() && "PCM file path cannot be empty");

    auto unit = run_clang(
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
    if(unit.completed() || unit.fatal_error()) {
        out.deps = unit.deps();
        // deps() collects include targets only; the module source is a
        // build input of its PCM all the same. Canonicalize it like every
        // other dep — srcPath keeps the command line's raw spelling, which
        // consumers cannot stat reliably.
        out.deps.emplace_back(std::string(unit.file_path(unit.main_file())),
                              llvm::xxh3_64bits(unit.main_content()));
    }
    return unit;
}

CompilationUnit complete(CompilationParams& params, clang::CodeCompleteConsumer* consumer) {
    auto& [file, offset] = params.completion;

    auto buffer = params.buffers.find(file);
    assert(buffer != params.buffers.end() && "completion file must be remapped");
    llvm::StringRef content = buffer->second->getBuffer();
    auto completion_offset =
        static_cast<std::uint32_t>(std::min<std::size_t>(offset, content.size()));
    auto position = kota::ipc::lsp::to_position({content.data(), content.size()},
                                                completion_offset,
                                                kota::ipc::lsp::PositionEncoding::UTF8);
    assert(position && "clamped completion offset must be mappable");

    /// Clang completion locations are 1-based.
    auto line = position->line + 1;
    auto column = position->character + 1;

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

std::string collect_errors(CompilationUnit& unit) {
    std::string errors;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.level >= DiagnosticLevel::Error) {
            if(!errors.empty())
                errors += "; ";
            errors += diag.message;
        }
    }
    return errors;
}

}  // namespace clice
