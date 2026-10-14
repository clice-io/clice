module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.command;
import :command.invocation;
import :compile.compilation;
import :compile.diagnostic;
import :compile.identity;
import :compile.semantics;
import :compile.tidy;
import :compile.tokens;
import :semantic.decls;
import :support.logging;
import :vfs.file_system;

namespace clice {

CompilationUnit::CompilationUnit(CompilationParams params) : params(std::move(params)) {
    using namespace std::chrono;
    build_time = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
}

CompilationUnit::~CompilationUnit() {
    if(action) {
        // We already notified the pp of end-of-file earlier, so detach it first.
        // We must keep it alive until after EndSourceFile(), Sema relies on this.
        std::shared_ptr<clang::Preprocessor> pp = instance->getPreprocessorPtr();
        // Detach so we don't send EOF again
        instance->setPreprocessor(nullptr);
        action->EndSourceFile();
    }
}

static bool writes_output(CompilationKind kind) {
    return kind == CompilationKind::Preamble || kind == CompilationKind::ModuleInterface;
}

std::unique_ptr<clang::CompilerInvocation>
    CompilationUnit::create_invocation(clang::DiagnosticConsumer* consumer) {
    auto& input = params.input;
    if(input.arguments.empty()) {
        LOG_ERROR_RET(nullptr, "Fail to create invocation: empty argument list from database");
    }

    /// Temporary diagnostic engine, only used for command line parsing.
    /// For compilation, we need to create a new diagnostic engine. See also
    /// https://github.com/llvm/llvm-project/pull/139584#issuecomment-2920704282.
    clang::DiagnosticOptions options;
    llvm::IntrusiveRefCntPtr diagnostic_engine =
        clang::CompilerInstance::createDiagnostics(*input.vfs, options, consumer, false);
    if(!diagnostic_engine) {
        LOG_ERROR_RET(nullptr, "Fail to create diagnostics engine");
    }

    std::vector<const char*> arguments;
    arguments.reserve(input.arguments.size());
    for(auto& argument: input.arguments) {
        arguments.push_back(argument.c_str());
    }
    auto invocation =
        create_compiler_invocation(arguments, input.directory, input.vfs, diagnostic_engine);
    if(!invocation) {
        LOG_ERROR_RET(nullptr,
                      " Fail to create invocation, arguments list is: {}",
                      print_argv(arguments));
    }

    auto& pp_opts = invocation->getPreprocessorOpts();

    // CompilerInstance does not deterministically clear RetainRemappedFileBuffers,
    // especially if compilation aborts early, so we keep them alive and clean up
    // in CompilationUnit's destructor instead.
    pp_opts.RetainRemappedFileBuffers = true;

    for(auto& [file, text]: input.buffers) {
        auto& buffer = remapped_buffers.emplace_back(llvm::MemoryBuffer::getMemBuffer(text));
        pp_opts.addRemappedFile(file, buffer.get());
    }
    if(params.kind != CompilationKind::Preamble) {
        llvm::append_range(pp_opts.Includes, input.forced_includes);
    }

    auto& [pch, bound] = input.pch;
    // Tests stand in for a PCH damaged past clang's checks, which crashes
    // the parse reading it, by writing CLICE_TEST_DAMAGE into it.
    const static auto damage = llvm::sys::Process::GetEnv("CLICE_TEST_DAMAGE");
    if(damage && !pch.empty()) {
        if(auto file = vfs::read(pch, vfs::Read::Bytes);
           file && (*file)->getBuffer().contains(*damage)) {
            LLVM_BUILTIN_TRAP;
        }
    }
    pp_opts.ImplicitPCHInclude = pch;
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

    auto& header_search_opts = invocation->getHeaderSearchOpts();
    header_search_opts.Verbose = false;
    for(auto& [name, path]: input.pcms) {
        header_search_opts.PrebuiltModuleFiles.try_emplace(name.str(), path);
    }

    // The driver caps every command at 20 errors, past which clang raises
    // a fatal error and instantiates no more templates: a file mid-edit
    // would lose the rest of its AST.
    invocation->getDiagnosticOpts().ErrorLimit = 0;

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

    // MSVC targets defer template bodies by default so SDK headers written
    // for MSVC's lookup rules still parse, but a deferred body has no AST and
    // every feature inside it goes blind. Only PCH and PCM builds keep the
    // default: an error there discards the PCH/PCM, so those headers must
    // keep parsing the way clang meant them to.
    if(!writes_output(params.kind)) {
        lang_opts.DelayedTemplateParsing = false;
    }

    // A header compiled under a source's command (`-x c++` buys a parse
    // instead of a precompiled-header job) is still a header: no "#pragma
    // once in main file", no unused warnings for its static functions.
    if(is_header_path(front_opts.Inputs[0].getFile())) {
        lang_opts.IsHeaderFile = true;
    }

    switch(params.kind) {
        case CompilationKind::Preamble: {
            front_opts.OutputFile = params.output_file;
            front_opts.ProgramAction = clang::frontend::GeneratePCH;
            pp_opts.GeneratePreamble = true;
            // Without recorded mtimes clang checks each input by its size
            // alone. Freshness is the master's call, made on content: a
            // same-bytes rewrite (`git stash pop`, a branch switch) moves
            // only the mtime and must not make clang reject a PCH the master
            // still considers fresh. The size check stays, as it guards the
            // reader against offsets past the end of a shrunk file.
            front_opts.IncludeTimestamps = false;
            lang_opts.CompilingPCH = true;
            break;
        }

        case CompilationKind::ModuleInterface: {
            front_opts.OutputFile = params.output_file;
            front_opts.ProgramAction = clang::frontend::GenerateReducedModuleInterface;
            break;
        }

        case CompilationKind::Preprocess:
        case CompilationKind::Content:
        case CompilationKind::Whole:
        case CompilationKind::Completion: {
            front_opts.OutputFile.clear();
            break;
        }
    }

    return invocation;
}

void CompilationUnit::run_tidy() {
    if(!checker) {
        return;
    }
    // The editor matches the main file's declarations alone: the
    // preamble's would cost a walk over every header. The unit's later
    // readers see the whole TU again, and the parent map built over the
    // restricted scope goes with it.
    auto& context = instance->getASTContext();
    if(!checker->batch) {
        context.setTraversalScope(collected_decls);
    }
    // Checks re-lex through the preprocessor (modernize-use-trailing-return-type
    // does): what the directive collector records meanwhile is no directive
    // of the file.
    auto collected = std::move(collected_directives);

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
    collected_directives = std::move(collected);
    context.setTraversalScope({context.getTranslationUnitDecl()});
}

/// A wrapper ast consumer, so that we can cancel the ast parse
class CompilationUnit::ParseConsumer final : public clang::MultiplexConsumer {
public:
    ParseConsumer(std::unique_ptr<clang::ASTConsumer> consumer, CompilationUnit& unit) :
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
                unit.collected_decls.push_back(record);
            }
        }

        unit.collected_decls.push_back(decl);
    }

    auto HandleTopLevelDecl(clang::DeclGroupRef group) -> bool final {
        if(unit.params.kind == CompilationKind::Content) {
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
        if(unit.params.stop->load()) {
            return false;
        }

        return clang::MultiplexConsumer::HandleTopLevelDecl(group);
    }

    // Sema adds an explicit instantiation directive's decl to its declaration
    // context and never hands it to the consumer, so the ones at file scope
    // are picked up here (a namespace block brings its own along).
    // noload_decls leaves a preamble's declarations on disk.
    void HandleTranslationUnit(clang::ASTContext& context) final {
        if(unit.params.kind == CompilationKind::Content) {
            for(auto* decl: context.getTranslationUnitDecl()->noload_decls()) {
                if(llvm::isa<clang::ExplicitInstantiationDecl>(decl)) {
                    collect_decl(decl);
                }
            }
        }
        // The PCH writer stores each import with the location where the
        // preprocessor made its module visible, and the reader skips the
        // imports without one; a C++20 import is made visible by Sema alone.
        if(unit.params.kind == CompilationKind::Preamble) {
            auto& pp = unit.instance->getPreprocessor();
            for(auto* import: context.local_imports()) {
                pp.makeModuleVisible(import->getImportedModule(), import->getLocation());
            }
        }
        clang::MultiplexConsumer::HandleTranslationUnit(context);
    }

private:
    CompilationUnit& unit;
    llvm::SmallPtrSet<const clang::Decl*, 8> collected;
};

class CompilationUnit::ParseAction final : public clang::WrapperFrontendAction {
public:
    ParseAction(std::unique_ptr<clang::FrontendAction> action, CompilationUnit& unit) :
        clang::WrapperFrontendAction(std::move(action)), unit(unit) {}

    auto CreateASTConsumer(clang::CompilerInstance& instance, llvm::StringRef file)
        -> std::unique_ptr<clang::ASTConsumer> final {
        auto consumer = WrapperFrontendAction::CreateASTConsumer(instance, file);
        if(!consumer)
            return nullptr;
        return std::make_unique<ParseConsumer>(std::move(consumer), unit);
    }

    /// Make this public.
    using clang::WrapperFrontendAction::EndSourceFile;

private:
    CompilationUnit& unit;
};

static std::unique_ptr<clang::FrontendAction> frontend_action(CompilationKind kind) {
    switch(kind) {
        case CompilationKind::Preprocess: return std::make_unique<clang::PreprocessOnlyAction>();
        case CompilationKind::Preamble: return std::make_unique<clang::GeneratePCHAction>();
        case CompilationKind::ModuleInterface:
            return std::make_unique<clang::GenerateReducedModuleInterfaceAction>();
        case CompilationKind::Content:
        case CompilationKind::Whole:
        case CompilationKind::Completion: return std::make_unique<clang::SyntaxOnlyAction>();
    }
    std::unreachable();
}

/// Point clang's code completion at `offset` in the main file.
static void complete_at(clang::CompilerInstance& instance, std::uint32_t offset) {
    auto& SM = instance.getSourceManager();
    auto main = SM.getMainFileID();
    auto content = SM.getBufferData(main);
    auto clamped = static_cast<std::uint32_t>(std::min<std::size_t>(offset, content.size()));
    auto lines = kota::ipc::lsp::line_starts({content.data(), content.size()});
    auto position = PositionMap(content, lines).position(clamped, PositionEncoding::UTF8);
    assert(position && "clamped completion offset must be mappable");

    /// Clang completion locations are 1-based.
    auto& at = instance.getFrontendOpts().CodeCompletionAt;
    at.FileName = SM.getFileEntryRefForID(main)->getName().str();
    at.Line = position->line + 1;
    at.Column = position->character + 1;
}

CompilationStatus CompilationUnit::run() {
    auto diagnostic_consumer = diagnostic_collector();
    auto invocation = create_invocation(diagnostic_consumer.get());
    if(!invocation) {
        return CompilationStatus::SetupFail;
    }

    instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    instance->createVirtualFileSystem(params.input.vfs, diagnostic_consumer.get());
    instance->createDiagnostics(diagnostic_consumer.release(), true);
    // A missing header is fatal to clang, which then reports nothing after it.
    instance->getDiagnostics().setSeverity(clang::diag::err_pp_file_not_found,
                                           clang::diag::Severity::Error,
                                           {});
    instance->createFileManager();

    if(!instance->createTarget()) {
        return CompilationStatus::SetupFail;
    }

    if(params.kind == CompilationKind::Completion) {
        assert(params.completion.consumer && "a completion needs its consumer");
        instance->setCodeCompletionConsumer(params.completion.consumer.release());
    }

    action = std::make_unique<ParseAction>(frontend_action(params.kind), *this);

    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        /// If the action is not empty, we will call `EndSourceFile` in the destructor.
        /// But if we fail to `BeginSourceFile` we don't need to call `EndSourceFile`. So just
        /// reset it.
        action.reset();
        return CompilationStatus::SetupFail;
    }

    if(params.kind == CompilationKind::Completion) {
        complete_at(*instance, params.completion.offset);
    }

    /// FIXME: include-fixer, etc?

    /// The checks' PPCallbacks go first: the preamble replay reaches the
    /// callbacks registered before it (see tidy::configure).
    if(params.tidy) {
        checker = tidy::configure(*instance,
                                  *params.tidy,
                                  params.kind != CompilationKind::Content,
                                  params.preamble_inactive_regions);
    }

    /// Add PPCallbacks to collect preprocessing information.
    instance->getPreprocessor().addPPCallbacks(directive_collector());

    if(params.kind == CompilationKind::Content) {
        collected_tokens.emplace(instance->getPreprocessor());
    }

    // Upstream FrontendAction::Execute() always returns success: errors go
    // through diagnostics.
    llvm::cantFail(action->Execute());

    if(params.stop->load()) {
        return CompilationStatus::Cancelled;
    }

    auto& diagnostics = instance->getDiagnostics();
    fatal = diagnostics.hasFatalErrorOccurred();
    /// A PCM build with errors writes nothing. A PCH is written in spite of
    /// its preamble's errors; clang writes none only after a module failed
    /// to load.
    if(params.kind == CompilationKind::Preamble) {
        output_ready = !instance->hadModuleLoaderFatalFailure();
    } else if(params.kind == CompilationKind::ModuleInterface) {
        output_ready = !diagnostics.hasErrorOccurred();
    }

    if(collected_tokens) {
        collected_tokens->finish();
    }

    run_tidy();

    if(instance->hasASTContext()) {
        template_resolver.emplace(instance->getASTContext());
    }

    return CompilationStatus::Completed;
}

std::unique_ptr<CompilationUnit> compile(CompilationParams params) {
    assert(writes_output(params.kind) != params.output_file.empty() &&
           "PCH and PCM builds, and only they, write an output file");

    // Record the stack bottom so that clang's own recursion guards
    // (`clang::runWithSufficientStackSpace` in the parser, constant
    // evaluator, etc.) actually work. They are inert without it and
    // deeply nested code would overflow the stack during parsing.
    clang::noteBottomOfStack();

    std::unique_ptr<CompilationUnit> unit(new CompilationUnit(std::move(params)));
    LOG_DEBUG("Compile command: {}", llvm::join(unit->params.input.arguments, " "));
    unit->outcome = unit->run();
    return unit;
}

std::vector<std::string> collect_errors(CompilationUnit& unit) {
    std::vector<std::string> errors;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.level >= DiagnosticLevel::Error) {
            errors.push_back(diag.message);
        }
    }
    return errors;
}

}  // namespace clice
