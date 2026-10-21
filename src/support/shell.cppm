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
    // As sh and clang's compilation database read it: a backslash escapes
    // the character after it outside single quotes and nothing inside them,
    // where LLVM's GNU tokenizer would take `'a\\b'` for `a\b`.
    std::string argument;
    bool open = false;
    for(std::size_t i = 0; i < command.size(); i += 1) {
        char c = command[i];
        if(llvm::isSpace(c)) {
            if(open) {
                arguments.push_back(saver.save(argument).data());
                argument.clear();
                open = false;
            }
            continue;
        }
        open = true;
        if(c == '\'') {
            auto end = std::min(command.find('\'', i + 1), command.size());
            argument += command.slice(i + 1, end);
            i = end;
        } else if(c == '"') {
            for(i += 1; i < command.size() && command[i] != '"'; i += 1) {
                if(command[i] == '\\' && i + 1 < command.size()) {
                    i += 1;
                }
                argument += command[i];
            }
        } else if(c == '\\' && i + 1 < command.size()) {
            i += 1;
            argument += command[i];
        } else {
            argument += c;
        }
    }
    if(open) {
        arguments.push_back(saver.save(argument).data());
    }
#endif
    return arguments;
}

/// `arguments` as one command line, each quoted as the host's shell and
/// tokenize_command read it back: what a person copies to run the command.
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
        // A backslash escapes the character after it, outside quotes and
        // in LLVM's reading inside them too, so it alone quotes for both.
        // Bytes past ASCII are UTF-8, which neither reads specially.
        if(argument.empty()) {
            line += "''";
            continue;
        }
        for(char c: argument) {
            if(!llvm::isAlnum(c) && !llvm::StringRef("@%+=:,./_-").contains(c) &&
               static_cast<unsigned char>(c) < 0x80) {
                line += '\\';
            }
            line += c;
        }
#endif
    }
    return line;
}

}  // namespace clice
