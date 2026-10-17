module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.command;
import :command.invocation;
import :compile.compilation;
import :compile.diagnostic;
import :compile.tidy;
import :compile.tokens;
import :support.logging;
import :syntax.scan;
import :vfs.file_system;

namespace clice {

CompilationUnit::CompilationUnit(CompilationParams params) : params(std::move(params)) {
    using namespace std::chrono;
    build_time = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
}

CompilationUnit::~CompilationUnit() {
    derived.clear();
    if(action) {
        // We already notified the pp of end-of-file earlier, so detach it first.
        // We must keep it alive until after EndSourceFile(), Sema relies on this.
        std::shared_ptr<clang::Preprocessor> pp = instance->getPreprocessorPtr();
        // Detach so we don't send EOF again
        instance->setPreprocessor(nullptr);
        action->EndSourceFile();
    }
}

static bool writes_pch(CompilationKind kind) {
    return kind == CompilationKind::Preamble || kind == CompilationKind::PrecompiledHeader;
}

static bool writes_output(CompilationKind kind) {
    return writes_pch(kind) || kind == CompilationKind::ModuleInterface;
}

std::unique_ptr<clang::CompilerInvocation>
    CompilationUnit::create_invocation(clang::DiagnosticConsumer* consumer) {
    auto& input = params.input;
    if(input.arguments.empty()) {
        LOG_ERROR("Fail to create invocation: empty argument list from database");
        return nullptr;
    }

    /// Temporary diagnostic engine, only used for command line parsing.
    /// For compilation, we need to create a new diagnostic engine. See also
    /// https://github.com/llvm/llvm-project/pull/139584#issuecomment-2920704282.
    clang::DiagnosticOptions options;
    llvm::IntrusiveRefCntPtr diagnostic_engine =
        clang::CompilerInstance::createDiagnostics(*input.vfs, options, consumer, false);
    if(!diagnostic_engine) {
        LOG_ERROR("Fail to create diagnostics engine");
        return nullptr;
    }

    std::vector<const char*> arguments;
    arguments.reserve(input.arguments.size());
    for(auto& argument: input.arguments) {
        arguments.push_back(argument.c_str());
    }
    LOG_DEBUG("Compile command: {}", print_argv(arguments));
    auto invocation =
        create_compiler_invocation(arguments, input.directory, input.vfs, diagnostic_engine);
    if(!invocation) {
        LOG_ERROR(" Fail to create invocation, arguments list is: {}", print_argv(arguments));
        return nullptr;
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
        case CompilationKind::Preamble:
        case CompilationKind::PrecompiledHeader: {
            front_opts.OutputFile = params.output_file;
            front_opts.ProgramAction = clang::frontend::GeneratePCH;
            pp_opts.GeneratePreamble = params.kind == CompilationKind::Preamble;
            // Without recorded mtimes clang checks each input by its size
            // alone. Freshness is the master's call, made on content: a
            // same-bytes rewrite (`git stash pop`, a branch switch) moves
            // only the mtime and must not make clang reject a PCH the master
            // still considers fresh. The size check stays, as it guards the
            // reader against offsets past the end of a shrunk file.
            front_opts.IncludeTimestamps = false;
            lang_opts.CompilingPCH = true;
            // The function templates the preamble uses are instantiated once,
            // here, instead of at the end of every parse loading the PCH.
            lang_opts.PCHInstantiateTemplates = instantiate;
            break;
        }

        case CompilationKind::ModuleInterface: {
            front_opts.OutputFile = params.output_file;
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
    // The editor matches the focus file's declarations alone: the
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

    // Tests crash the pass over a focus file holding CLICE_TEST_TIDY_CRASH.
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

/// Sema hands the functions it instantiates to the consumer too.
static bool implicitly_instantiated(const clang::Decl* decl) {
    auto kind = clang::TSK_Undeclared;
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        kind = function->getTemplateSpecializationKind();
    } else if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        kind = record->getTemplateSpecializationKind();
    } else if(auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        kind = var->getTemplateSpecializationKind();
    }
    return kind == clang::TSK_ImplicitInstantiation;
}

/// Whether Sema made `decl` from a template: the body of a function or the
/// initializer of a variable, implicitly or for an explicit instantiation,
/// a declaration's included (an inline function's body is still
/// instantiated under one); or the definition of a class.
static bool instantiated(const clang::Decl* decl) {
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return clang::isTemplateInstantiation(function->getTemplateSpecializationKind()) &&
               function->doesThisDeclarationHaveABody();
    }
    if(auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return clang::isTemplateInstantiation(var->getTemplateSpecializationKind());
    }
    if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        return clang::isTemplateInstantiation(record->getTemplateSpecializationKind());
    }
    return false;
}

/// A wrapper ast consumer, so that we can cancel the ast parse
class CompilationUnit::ParseConsumer final : public clang::MultiplexConsumer {
public:
    ParseConsumer(std::unique_ptr<clang::ASTConsumer> consumer, CompilationUnit& unit) :
        clang::MultiplexConsumer(std::move(consumer)), unit(unit) {}

    void collect_decl(clang::Decl* decl) {
        if(!unit.focus_file().contains(unit.produced_at(decl->getLocation()).file) &&
           !unit.encloses_focus_file(decl)) {
            return;
        }

        if(implicitly_instantiated(decl)) {
            return;
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

    /// Sema hands every definition it instantiates to the consumer: a
    /// function's or a variable template's here, a static data member's
    /// through HandleCXXStaticMemberVarInstantiation, a class's through
    /// HandleTagDeclDefinition. An explicit instantiation hands over again
    /// what an earlier use already instantiated.
    void collect_instantiation(clang::Decl* decl) {
        if(unit.params.kind != CompilationKind::Whole &&
           unit.params.kind != CompilationKind::Content) {
            return;
        }
        if(!instantiated(decl) || !instantiations.insert(decl).second) {
            return;
        }
        unit.collected_instantiations.push_back(decl);
    }

    auto HandleTopLevelDecl(clang::DeclGroupRef group) -> bool final {
        for(auto decl: group) {
            if(unit.params.kind == CompilationKind::Content) {
                collect_decl(decl);
            }
            collect_instantiation(decl);
        }

        /// TODO: check atomic variable after the parse of each declaration
        /// may result in performance issue, benchmark in the future.
        if(unit.params.stop->load()) {
            return false;
        }

        return clang::MultiplexConsumer::HandleTopLevelDecl(group);
    }

    void HandleCXXStaticMemberVarInstantiation(clang::VarDecl* decl) final {
        collect_instantiation(decl);
        clang::MultiplexConsumer::HandleCXXStaticMemberVarInstantiation(decl);
    }

    void HandleTagDeclDefinition(clang::TagDecl* decl) final {
        collect_instantiation(decl);
        clang::MultiplexConsumer::HandleTagDeclDefinition(decl);
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
        if(writes_pch(unit.params.kind)) {
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
    llvm::DenseSet<const clang::Decl*> instantiations;
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

/// Whether clang refused the unit's precompiled header for the options it
/// was built under, which conflict with the unit's own — not for its
/// bytes, a failure the master must still see to retract the PCH.
static bool refused_for_options(CompilationUnit& unit) {
    return llvm::any_of(unit.diagnostics(), [](const Diagnostic& diag) {
        if(diag.id.source != DiagnosticSource::Clang) {
            return false;
        }
        switch(diag.id.value) {
            case clang::diag::err_ast_file_codegenopt_mismatch:
            case clang::diag::err_ast_file_codegenopt_value_mismatch:
            case clang::diag::err_ast_file_diagopt_mismatch:
            case clang::diag::err_ast_file_langopt_mismatch:
            case clang::diag::err_ast_file_langopt_value_mismatch:
            case clang::diag::err_ast_file_macro_def_conflict:
            case clang::diag::err_ast_file_macro_def_undef:
            case clang::diag::err_ast_file_pp_detailed_record:
            case clang::diag::err_ast_file_targetopt_feature_mismatch:
            case clang::diag::err_ast_file_targetopt_mismatch:
            case clang::diag::err_ast_file_undef: return true;
            default: return false;
        }
    });
}

static std::unique_ptr<clang::FrontendAction> frontend_action(CompilationKind kind,
                                                              bool reduced_bmi) {
    switch(kind) {
        case CompilationKind::Preprocess: return std::make_unique<clang::PreprocessOnlyAction>();
        case CompilationKind::Preamble:
        case CompilationKind::PrecompiledHeader:
            return std::make_unique<clang::GeneratePCHAction>();
        case CompilationKind::ModuleInterface:
            if(reduced_bmi) {
                return std::make_unique<clang::GenerateReducedModuleInterfaceAction>();
            }
            return std::make_unique<clang::GenerateModuleInterfaceAction>();
        case CompilationKind::Content:
        case CompilationKind::Whole:
        case CompilationKind::Completion: return std::make_unique<clang::SyntaxOnlyAction>();
    }
    std::unreachable();
}

/// Point clang's code completion at `offset` in the focus file.
static void complete_at(clang::CompilerInstance& instance, std::uint32_t offset) {
    auto& SM = instance.getSourceManager();
    auto focus = SM.getMainFileID();
    auto content = SM.getBufferData(focus);
    auto clamped = static_cast<std::uint32_t>(std::min<std::size_t>(offset, content.size()));
    auto lines = kota::ipc::lsp::line_starts({content.data(), content.size()});
    auto position = PositionMap(content, lines).position(clamped, PositionEncoding::UTF8);
    assert(position && "clamped completion offset must be mappable");

    /// Clang completion locations are 1-based.
    auto& at = instance.getFrontendOpts().CodeCompletionAt;
    at.FileName = SM.getFileEntryRefForID(focus)->getName().str();
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

    action = std::make_unique<ParseAction>(
        frontend_action(params.kind, instance->getFrontendOpts().GenReducedBMI),
        *this);

    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        /// If the action is not empty, we will call `EndSourceFile` in the destructor.
        /// But if we fail to `BeginSourceFile` we don't need to call `EndSourceFile`. So just
        /// reset it.
        action.reset();
        return CompilationStatus::SetupFail;
    }

    // Handed over only now: a compile that could not start keeps it for
    // another try (see compile()).
    if(params.kind == CompilationKind::Completion) {
        assert(params.completion.consumer && "a completion needs its consumer");
        instance->setCodeCompletionConsumer(params.completion.consumer.release());
        complete_at(*instance, params.completion.offset);
    }

    // The PCH files what its build learned of the focus file under the
    // preamble's size, which the whole file does not have: a header's
    // `#pragma once` in its preamble is lost with it, and an include of the
    // header past the preamble, through another header, enters it again.
    if(auto& [pch, bound] = params.input.pch;
       !pch.empty() && instance->getLangOpts().IsHeaderFile) {
        auto& SM = instance->getSourceManager();
        auto focus = SM.getMainFileID();
        if(pragma_once(SM.getBufferData(focus), bound)) {
            instance->getPreprocessor().getHeaderSearchInfo().MarkFileIncludeOnce(
                *SM.getFileEntryRefForID(focus));
        }
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

    // Past the end of the focus file clang instantiates the templates the
    // preamble uses, and checks little else: the end of the last
    // declaration's body, an unclosed `#pragma clang attribute`. Their
    // errors cost a needless rebuild (see compile()).
    if(writes_pch(params.kind) && instantiate) {
        instance->getPreprocessor().setTokenWatcher([this](const clang::Token& token) {
            if(token.is(clang::tok::eof) && !errors_at_end) {
                errors_at_end = instance->getDiagnostics().getNumErrors();
            }
        });
    }

    if(params.kind == CompilationKind::Content) {
        collected_tokens.emplace(instance->getPreprocessor());
    }

    // Upstream FrontendAction::Execute() always returns success: errors go
    // through diagnostics.
    llvm::cantFail(action->Execute());

    if(params.stop->load()) {
        return CompilationStatus::Cancelled;
    }

    /// A PCM build with errors writes nothing. A preamble's PCH is written
    /// in spite of its errors; clang writes none only after a module failed
    /// to load. A precompiled header with errors serves no file: its
    /// consumers, compiled over it, would never report them.
    if(params.kind == CompilationKind::Preamble) {
        output_ready = !instance->hadModuleLoaderFatalFailure();
    } else if(params.kind == CompilationKind::ModuleInterface ||
              params.kind == CompilationKind::PrecompiledHeader) {
        output_ready = !instance->getDiagnostics().hasErrorOccurred();
    }

    if(collected_tokens) {
        collected_tokens->finish();
    }

    run_tidy();

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
    unit->outcome = unit->run();

    // A precompiled header the build declares that clang refuses for this
    // parse — the unit's own flags conflict with the ones it was built
    // under, where GCC takes the .gch for unusable and ignores it — leaves
    // the header the command forces in to parse what it holds.
    if(auto& [pch, bound] = unit->params.input.pch;
       unit->setup_fail() && !pch.empty() && bound == 0 && refused_for_options(*unit)) {
        LOG_WARN("Compiling without the precompiled header {}, which clang refused", pch);
        auto& built = unit->params;
        CompilationParams again{
            .kind = built.kind,
            .input = built.input,
            .output_file = built.output_file,
            .completion = std::move(built.completion),
            .tidy = built.tidy,
            .stop = built.stop,
        };
        again.input.pch = {};
        unit.reset();
        unit.reset(new CompilationUnit(std::move(again)));
        unit->outcome = unit->run();
    }

    // Instantiated where the preamble ends, a template misses what the focus
    // file declares later, which clang compiling the whole file sees: an
    // overload argument-dependent lookup would find, a type completed
    // there. When instantiating raised an error, the PCH is built again
    // without it.
    if(unit->completed() && unit->errors_at_end &&
       unit->instance->getDiagnostics().getNumErrors() > *unit->errors_at_end) {
        auto& built = unit->params;
        CompilationParams again{
            .kind = built.kind,
            .input = built.input,
            .output_file = built.output_file,
            .tidy = built.tidy,
            .stop = built.stop,
        };
        unit.reset();
        unit.reset(new CompilationUnit(std::move(again)));
        unit->instantiate = false;
        unit->outcome = unit->run();
    }
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
