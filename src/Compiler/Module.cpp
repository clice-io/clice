#include "Compiler/Module.h"
#include "Compiler/Compilation.h"

namespace clice {

std::string scanModuleName(CompilationParams& params) {
    /// Because [P3034](https://github.com/cplusplus/papers/issues/1696) has been
    /// accepted, the module name in module declaration cannot be a macro now.
    /// It means that if the module declaration doesn't occur in condition preprocess
    /// directive, we can determine the module name just by lexing the source file.
    clang::LangOptions langOpts;
    langOpts.Modules = true;
    langOpts.CPlusPlus20 = true;

    /// We use raw mode of lexer to avoid the preprocessor.
    clang::Lexer lexer(clang::SourceLocation(),
                       langOpts,
                       params.content.begin(),
                       params.content.begin(),
                       params.content.end());

    /// Whether we are in a condition directive.
    bool isInDirective = false;

    /// Whether we need to preprocess the source file.
    bool needPreprocess = false;

    std::string name;
    clang::Token token;
    while(true) {
        lexer.LexFromRawLexer(token);
        if(token.is(clang::tok::eof)) {
            break;
        }

        if(!token.isAtStartOfLine()) {
            continue;
        }

        if(token.is(clang::tok::hash)) {
            lexer.LexFromRawLexer(token);
            auto diretive = token.getRawIdentifier();
            if(diretive == "if" || diretive == "ifdef" || diretive == "ifndef") {
                isInDirective = true;
            } else if(diretive == "endif") {
                isInDirective = false;
            }
        } else if(token.is(clang::tok::raw_identifier)) {
            if(token.getRawIdentifier() != "export") [[likely]] {
                continue;
            }

            lexer.LexFromRawLexer(token);
            if(token.getRawIdentifier() != "module") {
                continue;
            }

            /// We are after `export module`.
            if(isInDirective) {
                /// If the module name occurs in a condition directive, we have
                /// to preprocess the source file to determine the module name.
                needPreprocess = true;
                break;
            }

            /// Otherwise, we can determine the module name directly.
            while(!lexer.LexFromRawLexer(token)) {
                auto kind = token.getKind();
                if(kind == clang::tok::raw_identifier) {
                    name += token.getRawIdentifier();
                } else if(kind == clang::tok::colon) {
                    name += ":";
                } else if(kind == clang::tok::period) {
                    name += ".";
                } else {
                    break;
                }
            }
            return name;
        } else {
            continue;
        }
    }

    /// If not need to preprocess and the function doesn't return, it means
    /// that this file is not a module interface unit.
    if(!needPreprocess) {
        return "";
    }

    auto info = scanModule(params);
    if(!info) {
        return "";
    }

    return info->name;
}

std::expected<ModuleInfo, std::string> scanModule(CompilationParams& params) {
    struct ModuleCollector : public clang::PPCallbacks {
        ModuleInfo& info;

        ModuleCollector(ModuleInfo& info) : info(info) {}

        void moduleImport(clang::SourceLocation importLoc,
                          clang::ModuleIdPath path,
                          const clang::Module* imported) override {
            assert(path.size() == 1);
            info.mods.emplace_back(path[0].first->getName());
        }
    };

    ModuleInfo info;
    clang::PreprocessOnlyAction action;
    auto instance = impl::createInstance(params);

    if(!action.BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        return std::unexpected("Failed to begin source file");
    }

    auto& pp = instance->getPreprocessor();

    pp.addPPCallbacks(std::make_unique<ModuleCollector>(info));

    if(auto error = action.Execute()) {
        return std::unexpected(std::format("{}", error));
    }

    if(pp.isInNamedModule()) {
        info.isInterfaceUnit = pp.isInNamedInterfaceUnit();
        info.name = pp.getNamedModuleName();
    }

    return info;
}

}  // namespace clice
