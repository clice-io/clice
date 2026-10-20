module;

#include "modules/prelude.h"

module clice:support.shell;

namespace clice {

/// Split a command line into arguments with the host's shell rules — the
/// rules a compile_commands.json `command` field is read with. The
/// arguments live in `saver`.
inline llvm::SmallVector<const char*, 32> tokenize_command(llvm::StringRef command,
                                                           llvm::StringSaver& saver) {
    llvm::SmallVector<const char*, 32> arguments;
#ifdef _WIN32
    llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
#else
    llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
#endif
    return arguments;
}

/// The command line tokenize_command splits back into `arguments`: what a
/// person copies into the host's shell to run the command.
inline std::string join_command(llvm::ArrayRef<std::string> arguments) {
    std::string line;
    for(llvm::StringRef argument: arguments) {
        if(!line.empty()) {
            line += ' ';
        }
#ifdef _WIN32
        // The rules of CommandLineToArgvW: backslashes are literal but
        // before a quote, where each one is escaped, as the quote is.
        if(!argument.empty() && argument.find_first_of(" \t\"") == llvm::StringRef::npos) {
            line += argument;
            continue;
        }
        line += '"';
        std::size_t backslashes = 0;
        for(char c: argument) {
            if(c == '"') {
                line.append(backslashes + 1, '\\');
            }
            backslashes = c == '\\' ? backslashes + 1 : 0;
            line += c;
        }
        line.append(backslashes, '\\');
        line += '"';
#else
        if(!argument.empty() && llvm::all_of(argument, [](char c) {
               return llvm::isAlnum(c) || llvm::StringRef("@%+=:,./_-").contains(c);
           })) {
            line += argument;
            continue;
        }
        line += '\'';
        for(char c: argument) {
            if(c == '\'') {
                line += R"('\'')";
            } else {
                line += c;
            }
        }
        line += '\'';
#endif
    }
    return line;
}

}  // namespace clice
