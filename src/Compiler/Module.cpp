#include "Compiler/Module.h"

#include "Compiler/Compilation.h"

#include "clang/Lex/Lexer.h"

namespace clice {

std::string scanModuleName(CompilationParams& params) {
    /// Because [P3034](https://github.com/cplusplus/papers/issues/1696) has been
    /// accepted, the module name in module declaration cannot be a macro now.
    /// It means that if the module declaration doesn't occur in condition preprocess
    /// directive, we can determine the module name just by lexing the source file.
    clang::LangOptions lang_opts;
    lang_opts.Modules = true;
    lang_opts.CPlusPlus20 = true;

    /// FIXME: Figure out main file from command line.
    assert(params.buffers.size() == 1);
    auto content = params.buffers.begin()->second->getBuffer();

    /// We use raw mode of lexer to avoid the preprocessor.
    clang::Lexer lexer(clang::SourceLocation(),
                       lang_opts,
                       content.begin(),
                       content.begin(),
                       content.end());

    /// Whether we are in a condition directive.
    bool is_in_directive = false;

    /// Whether we need to preprocess the source file.
    bool need_preprocess = false;

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
                is_in_directive = true;
            } else if(diretive == "endif") {
                is_in_directive = false;
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
            if(is_in_directive) {
                /// If the module name occurs in a condition directive, we have
                /// to preprocess the source file to determine the module name.
                need_preprocess = true;
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
    if(!need_preprocess) {
        return "";
    }

    auto info = scanModule(params);
    if(!info) {
        return "";
    }

    return info->name;
}

std::expected<ModuleInfo, std::string> scanModule(CompilationParams& params) {
    ModuleInfo info;
    auto unit = preprocess(params);
    if(!unit) {
        return std::unexpected(unit.error());
    }

    for(auto& import: unit->directives()[unit->interested_file()].imports) {
        info.mods.emplace_back(import.name);
    }

    info.isInterfaceUnit = unit->is_module_interface_unit();
    info.name = unit->module_name();

    return info;
}

}  // namespace clice
