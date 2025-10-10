#include "CompilationUnitImpl.h"
#include "Compiler/Command.h"
#include "Compiler/Compilation.h"
#include "Compiler/Diagnostic.h"
#include "Compiler/Tidy.h"
#include "Compiler/Utility.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/MultiplexConsumer.h"

#include "TidyImpl.h"

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
        auto location = decl->getLocation();
        if(location.isInvalid()) {
            return;
        }

        location = src_mgr.getExpansionLoc(location);
        auto fid = src_mgr.getFileID(location);
        if(fid == src_mgr.getPreambleFileID() || fid == src_mgr.getMainFileID()) {
            top_level_decls->push_back(decl);
        }
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
auto create_invocation(CompilationParams& params,
                       llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine>& diagnostic_engine)
    -> std::unique_ptr<clang::CompilerInvocation> {

    /// Create clang invocation.
    clang::CreateInvocationOptions options = {
        .Diags = diagnostic_engine,
        .VFS = params.vfs,

        /// Avoid replacing -include with -include-pch, also
        /// see https://github.com/clangd/clangd/issues/856.
        .ProbePrecompiled = false,
    };

    auto invocation = clang::createInvocation(params.arguments, options);
    if(!invocation) {
        return nullptr;
    }

    auto& pp_opts = invocation->getPreprocessorOpts();
    assert(!pp_opts.RetainRemappedFileBuffers && "RetainRemappedFileBuffers should be false");

    for(auto& [file, buffer]: params.buffers) {
        pp_opts.addRemappedFile(file, buffer.release());
    }
    params.buffers.clear();

    auto [pch, bound] = params.pch;
    pp_opts.ImplicitPCHInclude = std::move(pch);
    if(bound != 0) {
        pp_opts.PrecompiledPreambleBytes = {bound, false};
    }

    // We don't want to write comment locations into PCM. They are racy and slow
    // to read back. We rely on dynamic index for the comments instead.
    pp_opts.WriteCommentListToPCH = false;

    auto& header_search_opts = invocation->getHeaderSearchOpts();
    for(auto& [name, path]: params.pcms) {
        header_search_opts.PrebuiltModuleFiles.try_emplace(name.str(), std::move(path));
    }

    auto& front_opts = invocation->getFrontendOpts();
    front_opts.DisableFree = false;

    clang::LangOptions& langOpts = invocation->getLangOpts();
    langOpts.CommentOpts.ParseAllComments = true;
    langOpts.RetainCommentsFromSystemHeaders = true;

    return invocation;
}

/// Do nothing before or after compile state.
constexpr static auto no_hook = [](auto& /*ignore*/) {
};

template <typename Action,
          typename BeforeExecute = decltype(no_hook),
          typename AfterExecute = decltype(no_hook)>
CompilationResult run_clang(CompilationParams& params,
                            const BeforeExecute& before_execute = no_hook,
                            const AfterExecute& after_execute = no_hook) {
    auto diagnostics =
        params.diagnostics ? params.diagnostics : std::make_shared<std::vector<Diagnostic>>();
    auto [diagnostic_collector, diagnostic_client] = Diagnostic::create(diagnostics);
    auto diagnostic_engine =
        clang::CompilerInstance::createDiagnostics(*params.vfs,
                                                   new clang::DiagnosticOptions(),
                                                   diagnostic_client);

    auto invocation = create_invocation(params, diagnostic_engine);
    if(!invocation) {
        return std::unexpected("Fail to create compilation invocation!");
    }

    auto instance = std::make_unique<clang::CompilerInstance>();
    instance->setInvocation(std::move(invocation));
    instance->setDiagnostics(diagnostic_engine.get());

    if(auto remapping = clang::createVFSFromCompilerInvocation(instance->getInvocation(),
                                                               instance->getDiagnostics(),
                                                               params.vfs)) {
        instance->createFileManager(std::move(remapping));
    }

    if(!instance->createTarget()) {
        return std::unexpected("Fail to create target!");
    }

    /// Adjust the compiler instance, for example, set preamble or modules.
    before_execute(*instance);

    /// Frontend information ...
    std::vector<clang::Decl*> top_level_decls;
    llvm::DenseMap<clang::FileID, Directive> directives;
    std::optional<clang::syntax::TokenCollector> token_collector;

    auto action = std::make_unique<ProxyAction>(
        std::make_unique<Action>(),
        /// We only collect top level declarations for parse main file.
        (params.clang_tidy || params.kind == CompilationUnit::Content) ? &top_level_decls : nullptr,
        params.stop);

    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        return std::unexpected("Fail to begin source file");
    }

    auto& pp = instance->getPreprocessor();
    /// FIXME: include-fixer, etc?

    /// Setup clang-tidy
    std::unique_ptr<tidy::ClangTidyChecker> checker;
    if(params.clang_tidy) {
        tidy::TidyParams params;
        checker = tidy::configure(*instance, params);
        diagnostic_collector->set_transform(checker.get());
    }

    /// `BeginSourceFile` may create new preprocessor, so all operations related to preprocessor
    /// should be done after `BeginSourceFile`.
    Directive::attach(pp, directives);

    /// It is not necessary to collect tokens if we are running code completion.
    /// And in fact will cause assertion failure.
    if(!instance->hasCodeCompletionConsumer()) {
        token_collector.emplace(pp);
    }

    if(auto error = action->Execute()) {
        return std::unexpected(std::format("Failed to execute action, because {} ", error));
    }

    /// If the output file is not empty, it represents that we are
    /// generating a PCH or PCM. If error occurs, the AST must be
    /// invalid to some extent, serialization of such AST may result
    /// in crash frequently. So forbidden it here and return as error.
    if(!instance->getFrontendOpts().OutputFile.empty() &&
       instance->getDiagnostics().hasErrorOccurred()) {
        action->EndSourceFile();
        return std::unexpected("Fail to build PCH or PCM, error occurs in compilation.");
    }

    /// Check whether the compilation is canceled, if so we think
    /// it is an error.
    if(params.stop && params.stop->load()) {
        action->EndSourceFile();
        return std::unexpected("Compilation is canceled.");
    }

    std::optional<clang::syntax::TokenBuffer> token_buffer;
    if(token_collector) {
        token_buffer = std::move(*token_collector).consume();
    }

    // Must be called before EndSourceFile because the ast context can be destroyed later.
    if(checker) {
        auto clangd_top_level_decls = top_level_decls;
        std::erase_if(clangd_top_level_decls,
                      [](auto decl) { return !is_clangd_top_level_decl(decl); });
        // AST traversals should exclude the preamble, to avoid performance cliffs.
        // TODO: is it okay to affect the unit-level traversal scope here?
        instance->getASTContext().setTraversalScope(clangd_top_level_decls);
        checker->finder.matchAST(instance->getASTContext());
    }

    /// XXX: This is messy: clang-tidy checks flush some diagnostics at EOF.
    /// However Action->EndSourceFile() would destroy the ASTContext!
    /// So just inform the preprocessor of EOF, while keeping everything alive.
    pp.EndSourceFile();

    /// FIXME: getDependencies currently return ArrayRef<std::string>, which actually results in
    /// extra copy. It would be great to avoid this copy.

    std::optional<TemplateResolver> resolver;
    if(instance->hasSema()) {
        resolver.emplace(instance->getSema());
    }

    if(checker) {
        /// Avoid dangling pointer.
        diagnostic_collector->set_transform(nullptr);
    }

    auto impl = new CompilationUnit::Impl{
        .interested = instance->getSourceManager().getMainFileID(),
        .src_mgr = instance->getSourceManager(),
        .action = std::move(action),
        .instance = std::move(instance),
        .m_resolver = std::move(resolver),
        .buffer = std::move(token_buffer),
        .m_directives = std::move(directives),
        .pathCache = llvm::DenseMap<clang::FileID, llvm::StringRef>(),
        .symbolHashCache = llvm::DenseMap<const void*, std::uint64_t>(),
        .diagnostics = diagnostics,
        .top_level_decls = std::move(top_level_decls),
    };

    CompilationUnit unit(params.kind, impl);
    after_execute(unit);
    return unit;
}

}  // namespace

CompilationResult preprocess(CompilationParams& params) {
    return run_clang<clang::PreprocessOnlyAction>(params);
}

CompilationResult compile(CompilationParams& params) {
    return run_clang<clang::SyntaxOnlyAction>(params);
}

CompilationResult compile(CompilationParams& params, PCHInfo& out) {
    assert(!params.output_file.empty() && "PCH file path cannot be empty");

    /// Record the begin time of PCH building.
    auto now = std::chrono::system_clock::now().time_since_epoch();
    out.mtime = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    return run_clang<clang::GeneratePCHAction>(
        params,
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
        [&](CompilationUnit& unit) {
            out.path = params.output_file.str();
            out.preamble = unit.interested_content();
            out.deps = unit.deps();
            out.arguments = params.arguments;
        });
}

CompilationResult compile(CompilationParams& params, PCMInfo& out) {
    assert(!params.output_file.empty() && "PCM file path cannot be empty");

    return run_clang<clang::GenerateReducedModuleInterfaceAction>(
        params,
        [&](clang::CompilerInstance& instance) {
            /// Set options to generate PCH.
            instance.getFrontendOpts().OutputFile = params.output_file.str();
            instance.getFrontendOpts().ProgramAction =
                clang::frontend::GenerateReducedModuleInterface;

            out.srcPath = instance.getFrontendOpts().Inputs[0].getFile();
        },
        [&](CompilationUnit& unit) {
            out.path = params.output_file.str();

            for(auto& [name, path]: params.pcms) {
                out.mods.emplace_back(name);
            }
        });
}

CompilationResult complete(CompilationParams& params, clang::CodeCompleteConsumer* consumer) {
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

    return run_clang<clang::SyntaxOnlyAction>(params, [&](clang::CompilerInstance& instance) {
        /// Set options to run code completion.
        instance.getFrontendOpts().CodeCompletionAt.FileName = std::move(file);
        instance.getFrontendOpts().CodeCompletionAt.Line = line;
        instance.getFrontendOpts().CodeCompletionAt.Column = column;
        instance.setCodeCompletionConsumer(consumer);
    });
}

}  // namespace clice
