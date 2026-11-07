module clice:support.shell;

import :support.prelude;

namespace clice {

/// How the host's shell quotes a command line: what a compile_commands.json
/// `command` field is read with, but for one written inside Cygwin or MSYS2,
/// quoted for sh.
constexpr sys::cmd::Quoting host_quoting =
#ifdef _WIN32
    sys::cmd::Quoting::Windows;
#else
    sys::cmd::Quoting::POSIX;
#endif

/// The arguments of command line `command`, quoted as `quoting` says, into
/// `arguments`, whose strings keep their storage for the next line.
inline void split_command(llvm::StringRef command,
                          sys::cmd::Quoting quoting,
                          std::vector<std::string>& arguments) {
    std::size_t count = 0;
    // Blanks before the program are none of the command, as clang reads a
    // database's: Windows's rule would read them as an empty program name.
    for(auto argument: sys::cmd::split(command.ltrim(), quoting)) {
        if(count < arguments.size()) {
            arguments[count].assign(argument);
        } else {
            arguments.emplace_back(argument);
        }
        count += 1;
    }
    arguments.resize(count);
}

/// The arguments of command line `command` when it quotes and escapes
/// nothing and separates them by spaces alone, which every quoting reads
/// alike: views of it, into `arguments`. False for any other line.
inline bool split_plain(llvm::StringRef command,
                        llvm::SmallVectorImpl<std::string_view>& arguments) {
    arguments.clear();
    std::size_t start = 0;
    bool inside = false;
    for(std::size_t i = 0; i < command.size(); i += 1) {
        switch(command[i]) {
            case '"':
            case '\'':
            case '\\':
            case '\t':
            case '\n':
            case '\v':
            case '\f':
            case '\r': return false;
            case ' ':
                if(inside) {
                    arguments.emplace_back(command.data() + start, i - start);
                    inside = false;
                }
                break;
            default:
                if(!inside) {
                    start = i;
                    inside = true;
                }
        }
    }
    if(inside) {
        arguments.emplace_back(command.data() + start, command.size() - start);
    }
    return true;
}

/// The arguments of command line `command`, quoted as `quoting` says.
inline std::vector<std::string> split_command(llvm::StringRef command,
                                              sys::cmd::Quoting quoting = host_quoting) {
    std::vector<std::string> arguments;
    split_command(command, quoting, arguments);
    return arguments;
}

/// `arguments` as one command line, each quoted as the host's shell and
/// split_command read it back: what a person copies to run the command.
inline std::string join_command(kotatsu::string_list_ref arguments) {
    std::string line;
    sys::cmd::join(line, arguments, host_quoting);
    return line;
}

/// Format an argument list as one human-readable line.
inline std::string print_argv(kotatsu::string_list_ref args) {
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
