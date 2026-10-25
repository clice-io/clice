module;

#include "support/prelude.h"

module clice:support.shell;

namespace clice {

/// Split a command line into arguments as sh and clang's compilation
/// database read it: a backslash escapes the character after it outside
/// single quotes and nothing inside them, where LLVM's GNU tokenizer would
/// take `'a\\b'` for `a\b`. The arguments live in `saver`.
inline llvm::SmallVector<const char*, 32> tokenize_sh_command(llvm::StringRef command,
                                                              llvm::StringSaver& saver) {
    llvm::SmallVector<const char*, 32> arguments;
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
    return arguments;
}

/// Split a command line into arguments with the host's shell rules — the
/// rules a compile_commands.json `command` field is read with. The
/// arguments live in `saver`.
inline llvm::SmallVector<const char*, 32> tokenize_command(llvm::StringRef command,
                                                           llvm::StringSaver& saver) {
#ifdef _WIN32
    llvm::SmallVector<const char*, 32> arguments;
    llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
    return arguments;
#else
    return tokenize_sh_command(command, saver);
#endif
}

/// `arguments` as one command line, each quoted as the host's shell and
/// tokenize_command read it back: what a person copies to run the command.
inline std::string join_command(llvm::ArrayRef<std::string> arguments) {
    std::string line;
#ifdef _WIN32
    sys::windows::join_command_line(line, arguments);
#else
    for(llvm::StringRef argument: arguments) {
        if(!line.empty()) {
            line += ' ';
        }
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
    }
#endif
    return line;
}

/// Format an argument list as one human-readable line.
inline std::string print_argv(llvm::ArrayRef<const char*> args) {
    // Bytes past ASCII are UTF-8 and print as they are.
    auto control = [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
    };
    std::string buf;
    llvm::raw_string_ostream os(buf);
    bool sep = false;
    for(llvm::StringRef arg: args) {
        if(sep)
            os << ' ';
        sep = true;
        if(!ranges::any_of(arg, control) && arg.find_first_of(R"( "\)") == llvm::StringRef::npos) {
            os << arg;
            continue;
        }
        os << '"';
        for(char c: arg) {
            if(c == '"' || c == '\\') {
                os << '\\' << c;
            } else if(control(c)) {
                os << llvm::format(R"(\x%02X)", static_cast<unsigned char>(c));
            } else {
                os << c;
            }
        }
        os << '"';
    }
    return std::move(os.str());
}

}  // namespace clice
