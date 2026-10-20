module;

#include "modules/prelude.h"

module clice;

import :command.argument_parser;
import :command.driver_report;
import :vfs.path;

namespace clice {

namespace {

/// A path compared lexically: forward slashes, `.` and `..` folded.
std::string lexical(llvm::StringRef path) {
    llvm::SmallString<256> folded(path);
    std::ranges::replace(folded, '\\', '/');
    llvm::sys::path::remove_dots(folded, /*remove_dot_dot=*/true, llvm::sys::path::Style::posix);
    return folded.str().str();
}

/// The -std value for the macros a driver predefines: `__cplusplus` names
/// the C++ standard, `__STDC_VERSION__` the C one, and `__STRICT_ANSI__`
/// tells the ISO mode from the GNU one. A value between two standards
/// (GCC's c++2b is 202100L) counts as the newer. Empty when neither names
/// a standard: an assembler input, or C89, the default of no GCC since 5.
std::string standard_from_macros(llvm::StringRef cplusplus,
                                 llvm::StringRef stdc_version,
                                 bool strict) {
    auto value = [](llvm::StringRef macro) {
        std::uint64_t number = 0;
        macro.rtrim("lLuU").getAsInteger(10, number);
        return number;
    };
    auto pick = [](std::uint64_t number,
                   std::initializer_list<std::pair<std::uint64_t, llvm::StringRef>> table,
                   llvm::StringRef newest) {
        for(auto [bound, name]: table) {
            if(number <= bound) {
                return name;
            }
        }
        return newest;
    };
    if(!cplusplus.empty()) {
        auto year = pick(value(cplusplus),
                         {
                             {199711, "98"},
                             {201103, "11"},
                             {201402, "14"},
                             {201703, "17"},
                             {202002, "20"},
                             {202302, "23"}
        },
                         "26");
        return (strict ? "c++" : "gnu++") + year.str();
    }
    if(stdc_version.empty()) {
        return {};
    }
    auto year = pick(value(stdc_version),
                     {
                         {199901, "99"},
                         {201112, "11"},
                         {201710, "17"},
                         {202311, "23"}
    },
                     "2y");
    return (strict ? "c" : "gnu") + year.str();
}

/// The options of `line` when it runs GCC's compiler proper (cc1,
/// cc1plus, ...), else nothing. GCC prints the job unquoted: the program
/// ends where the first option starts.
std::optional<llvm::StringRef> gcc_job_options(llvm::StringRef line) {
    line = line.ltrim();
    auto end = line.find(" -");
    auto name = llvm::sys::path::stem(line.take_front(end), llvm::sys::path::Style::windows_slash);
    if(end == llvm::StringRef::npos || !name.starts_with("cc1")) {
        return std::nullopt;
    }
    return line.drop_front(end);
}

}  // namespace

DriverReport parse_driver_report(llvm::StringRef out, llvm::StringRef err) {
    DriverReport report;
    report.clang_job = parse_cc1(err);

    llvm::SmallVector<llvm::StringRef> lines;
    err.split(lines, '\n', -1, false);
    std::vector<std::string> library_path;
    bool in_search_list = false;
    for(auto line: lines) {
        line = line.rtrim("\r");
        if(in_search_list) {
            if(line.starts_with("End of search list.")) {
                in_search_list = false;
                continue;
            }
            auto dir = line.trim();
            bool framework = dir.consume_back(" (framework directory)");
            report.search_dirs.push_back({.path = dir.str(), .framework = framework});
        } else if(line.starts_with("#include <...> search starts here:")) {
            in_search_list = true;
        } else if(line.consume_front("Target: ")) {
            report.target = line.trim().str();
        } else if(line.consume_front("LIBRARY_PATH=")) {
            llvm::SmallVector<llvm::StringRef> dirs;
            line.split(dirs, path::list_separator, -1, false);
            for(auto dir: dirs) {
                library_path.push_back(lexical((dir + "/include").str()));
            }
        } else if(auto options = gcc_job_options(line); options && report.clang_job.empty()) {
            report.gcc_job = true;
            // The options kept are joined forms: splitting at spaces cuts
            // none of them.
            llvm::SmallVector<llvm::StringRef> tokens;
            llvm::SplitString(*options, tokens);
            for(auto token: tokens) {
                if(token.starts_with("-D") || token.starts_with("-U") || token.starts_with("-m") ||
                   token.starts_with("-f")) {
                    report.gcc_options.push_back(token.str());
                }
            }
        }
    }

    // The private directory holds GCC's builtin headers: it is the
    // `include` beside the libraries of GCC's own library directory, which
    // LIBRARY_PATH lists after any multilib variant (thumb/v7e-m+fp/hard/).
    for(std::size_t i = 0; i < report.search_dirs.size() && !report.private_dir; i += 1) {
        if(llvm::is_contained(library_path, lexical(report.search_dirs[i].path))) {
            report.private_dir = i;
        }
    }

    llvm::StringRef cplusplus;
    llvm::StringRef stdc_version;
    bool strict = false;
    bool opencl = false;
    llvm::SmallVector<llvm::StringRef> macros;
    out.split(macros, '\n', -1, false);
    for(auto macro: macros) {
        if(!macro.consume_front("#define ")) {
            continue;
        }
        auto [name, value] = macro.rtrim("\r").split(' ');
        if(name == "__cplusplus") {
            cplusplus = value;
        } else if(name == "__STDC_VERSION__") {
            stdc_version = value;
        } else if(name == "__STRICT_ANSI__") {
            strict = true;
        } else if(name == "__OPENCL_C_VERSION__" || name == "__OPENCL_CPP_VERSION__") {
            // OpenCL's version is -cl-std's; clang rejects any -std with it.
            opencl = true;
        }
    }
    if(!opencl) {
        report.standard = standard_from_macros(cplusplus, stdc_version, strict);
    }
    return report;
}

std::vector<std::string> parse_cc1(llvm::StringRef content) {
    llvm::SmallVector<llvm::StringRef> lines;
    content.split(lines, '\n', -1, false);

    for(llvm::StringRef line: lines) {
        line = line.trim();
        if(line.empty() || line.front() != '"')
            continue;

        llvm::SmallVector<const char*, 256> args;
        llvm::BumpPtrAllocator alloc;
        llvm::StringSaver saver(alloc);
        llvm::cl::TokenizeGNUCommandLine(line, saver, args);

        using namespace std::string_view_literals;
        // A multi-call llvm (LLVM_TOOL_LLVM_DRIVER_BUILD, as in xclang) runs
        // cc1 as `llvm clang -cc1`: the tool's name comes before -cc1.
        std::size_t cc1 = args.size() > 2 && args[1] != "-cc1"sv && args[2] == "-cc1"sv ? 2 : 1;
        if(args.size() <= cc1 || args[cc1] != "-cc1"sv)
            continue;

        std::vector<std::string> cc1_args;
        cc1_args.emplace_back(args[0]);
        cc1_args.emplace_back(args[cc1]);

        // Parse with CC1 visibility: the external driver may be newer than the
        // linked clang, so flags it emits that our cc1 does not understand
        // parse as unknown and are dropped. greedy_unknown makes an unknown
        // option consume its trailing values, so they are dropped along with
        // it instead of being misparsed as input files. Raw tokens are copied
        // through to preserve the exact spelling the driver emitted.
        // FIXME: Long-term we should unify the command pipeline so the driver
        // version always matches the embedded LLVM.
        std::vector<std::string> raw(args.begin() + cc1 + 1, args.end());
        auto options =
            kota::option::ParseOptions{.greedy_unknown = true, .visibility = option::CC1Option};
        for(auto& r: option::table().parse(raw, options)) {
            if(!r.has_value() || r->id == option::OPT_UNKNOWN)
                continue;
            // A newer external driver may emit known options with values our
            // linked clang cannot parse (e.g. -mframe-pointer=non-leaf-no-reserve).
            // Codegen options are irrelevant for syntax analysis; stripping them
            // avoids CompilerInvocation::CreateFromArgs failures on new values.
            if(is_codegen_option(r->id))
                continue;
            if(r->id == option::OPT_E) {
                cc1_args.emplace_back("-fsyntax-only");
                continue;
            }
            if(r->id == option::OPT_dM || r->id == option::OPT_v || r->id == option::OPT_o)
                continue;
            for(std::uint32_t i = r->index; i < r->next_index; i += 1)
                cc1_args.emplace_back(raw[i]);
        }
        return cc1_args;
    }
    return {};
}

}  // namespace clice
