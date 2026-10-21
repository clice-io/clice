module;

#include "modules/prelude.h"

module clice;

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
    report.clang_job = parse_clang_job(err);

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

CLReport parse_cl_report(llvm::StringRef out) {
    CLReport report;
    llvm::SmallVector<llvm::StringRef> lines;
    out.split(lines, '\n', -1, false);
    for(llvm::StringRef line: lines) {
        auto [label, value] = line.trim().split(' ');
        value = value.trim();
        std::uint64_t number = 0;
        if(value.rtrim("lL").getAsInteger(10, number)) {
            continue;
        }
        if(label == "msvc_lang") {
            report.standard = standard_from_macros(value, "", true);
        } else if(label == "msc_full_ver") {
            report.version = std::format("{}.{}.{}",
                                         number / 10'000'000,
                                         number / 100'000 % 100,
                                         number % 100'000);
        }
    }
    return report;
}

std::vector<std::string> parse_clang_job(llvm::StringRef output) {
    llvm::SmallVector<llvm::StringRef> lines;
    output.split(lines, '\n', -1, false);
    for(llvm::StringRef line: lines) {
        line = line.trim();
        if(!line.starts_with('"')) {
            continue;
        }
        llvm::SmallVector<const char*, 256> args;
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        llvm::cl::TokenizeGNUCommandLine(line, saver, args);

        // A multi-call llvm (LLVM_TOOL_LLVM_DRIVER_BUILD, as in xclang) runs
        // cc1 as `llvm clang -cc1`: the tool's name comes before -cc1.
        auto is_cc1 = [&](std::size_t i) {
            return i < args.size() && llvm::StringRef(args[i]) == "-cc1";
        };
        std::size_t cc1 = is_cc1(1) ? 1 : is_cc1(2) ? 2 : 0;
        if(cc1 == 0) {
            continue;
        }
        std::vector<std::string> job = {args[0]};
        job.insert(job.end(), args.begin() + cc1, args.end());
        return job;
    }
    return {};
}

}  // namespace clice
