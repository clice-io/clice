module;

#include "support/logging.macros.h"
#include "support/prelude.h"

module clice;

import :command.argument;
import :command.table;
import :command.toolchains.clang;
import :command.toolchains.gcc;
import :support.logging;
import :support.process;
import :support.shell;
import :vfs.cygwin;
import :vfs.path;

namespace clice {

namespace {

/// A path compared lexically: forward slashes, `.` and `..` folded.
std::string lexical(llvm::StringRef path) {
    llvm::SmallString<256> folded(path);
    ranges::replace(folded, '\\', '/');
    llvm::sys::path::remove_dots(folded, /*remove_dot_dot=*/true, llvm::sys::path::Style::posix);
    return folded.str().str();
}

/// The program and the options of `line` when it runs GCC's compiler
/// proper (cc1, cc1plus, ...), else nothing. GCC prints the job unquoted:
/// the program ends where the first option starts.
std::optional<std::pair<llvm::StringRef, llvm::StringRef>> gcc_job_line(llvm::StringRef line) {
    line = line.ltrim();
    auto end = line.find(" -");
    auto program = line.take_front(end);
    auto name = llvm::sys::path::stem(program, llvm::sys::path::Style::windows_slash);
    if(end == llvm::StringRef::npos || !name.starts_with("cc1")) {
        return std::nullopt;
    }
    return std::pair{program, line.drop_front(end)};
}

/// A Cygwin or MSYS2 driver reports its job's paths and its search list as
/// POSIX paths, which the mount table of the runtime it loads maps to the
/// Windows paths clang opens. Such a driver names its job's program
/// POSIX-style too: a native clang's job for a Linux target names
/// `/usr/include`, which is clang's to resolve, not a runtime's.
kota::task<std::expected<GCCStyleOutput, std::string>> translate_posix_paths(GCCStyleOutput output,
                                                                             std::string driver) {
    auto program = output.clang_job.empty() ? output.gcc_job : output.clang_job.front();
    if(!is_posix_path(program)) {
        co_return output;
    }
    auto table = co_await find_mount_table(driver, {program});
    if(!table) {
        co_return std::unexpected(
            std::format("{} reports POSIX paths ({}): {}", driver, program, table.error()));
    }
    for(auto& dir: output.search_dirs) {
        if(is_posix_path(dir.path)) {
            dir.path = table->to_windows(dir.path);
        }
    }
    for(auto& token: output.clang_job) {
        if(is_posix_path(token)) {
            token = table->to_windows(token);
        }
    }
    co_return output;
}

/// The extensions of a RISC-V ISA string after its `rv32` or `rv64`, each
/// with the version the string gives it, as the ISA string is written:
/// single letters may run together, versioned or not (`imac`, `i2p0m2p0`,
/// `gcv0p7`), and a multi-letter one (`zba`, a vendor's `xw`) runs to the
/// next `_`, glued to the letters before it or not (`rv32imacxw`).
llvm::SmallVector<llvm::StringRef> riscv_extensions(llvm::StringRef isa) {
    llvm::SmallVector<llvm::StringRef> parts;
    isa.split(parts, '_', -1, /*KeepEmpty=*/false);
    llvm::SmallVector<llvm::StringRef> extensions;
    for(auto part: parts) {
        while(!part.empty()) {
            if(llvm::StringRef("zsx").contains(part.front())) {
                extensions.push_back(part);
                break;
            }
            std::size_t length = 1;
            auto digits = [&] {
                auto start = length;
                while(length < part.size() && llvm::isDigit(part[length])) {
                    length += 1;
                }
                return length > start;
            };
            if(digits() && length + 1 < part.size() && part[length] == 'p' &&
               llvm::isDigit(part[length + 1])) {
                length += 1;
                digits();
            }
            extensions.push_back(part.take_front(length));
            part = part.drop_front(length);
        }
    }
    return extensions;
}

/// An extension without the version an ISA string may give it, `<major>` or
/// `<major>p<minor>` (`i2p0`, `zba1`).
llvm::StringRef unversioned(llvm::StringRef extension) {
    constexpr llvm::StringLiteral digits = "0123456789";
    auto name = extension.rtrim(digits);
    auto major = name;
    if(major.consume_back("p") && major.rtrim(digits).size() < major.size()) {
        return major.rtrim(digits);
    }
    return name;
}

/// Whether LLVM reads `isa` as clang's driver does: a profile
/// (`rva22u64`) as well as extensions.
bool parses(llvm::StringRef isa) {
    auto info = llvm::RISCVISAInfo::parseArchString(isa, /*EnableExperimentalExtension=*/false);
    if(!info) {
        llvm::consumeError(info.takeError());
        return false;
    }
    return true;
}

/// A RISC-V ISA string as clang can read it. clang drops a whole string
/// naming an extension LLVM does not know (a vendor's, WCH's `xw`), a
/// version of one it does not know (older GCCs' `i2p0`, where LLVM's I is
/// 2.1), or an extension LLVM has only as experimental (GCC 15's
/// `zicfilp`), and every feature macro of the rest with it. The second
/// takes the version LLVM knows, the others go; a string LLVM reads stays.
std::string riscv_isa_for_clang(llvm::StringRef isa) {
    if(parses(isa)) {
        return isa.str();
    }
    auto usable = [](llvm::StringRef extension) {
        auto feature = llvm::RISCVISAInfo::getTargetFeatureForExtension(extension);
        return !feature.empty() && !llvm::StringRef(feature).starts_with("experimental-");
    };
    auto known = [&](llvm::StringRef extension) -> std::optional<llvm::StringRef> {
        if(usable(extension)) {
            return extension;
        }
        auto name = unversioned(extension);
        if(name.size() < extension.size() && usable(name)) {
            LOG_INFO("LLVM knows no version {} of the RISC-V extension {}: clang reads its own",
                     extension.drop_front(name.size()),
                     name);
            return name;
        }
        return std::nullopt;
    };

    // The first part stays when LLVM reads it (`rv32imc`, a profile);
    // else its width does, and its base (`i`, `e`, `g`, which LLVM has as
    // no extension of its own) without a version LLVM does not know.
    auto [first, rest] = isa.split('_');
    std::string result;
    llvm::SmallVector<llvm::StringRef> extensions;
    if(parses(first)) {
        result = first.str();
        extensions = riscv_extensions(rest);
    } else {
        auto width = first.find_first_not_of("rv0123456789");
        result = first.take_front(width).str();
        extensions = riscv_extensions(isa.drop_front(width));
        if(!extensions.empty()) {
            result += known(extensions.front()).value_or(unversioned(extensions.front()));
            extensions.erase(extensions.begin());
        }
    }
    for(auto extension: extensions) {
        if(auto name = known(extension)) {
            result += '_';
            result += *name;
        } else {
            LOG_WARN(
                "clang takes no RISC-V extension {} (LLVM has none, or an experimental "
                "one): it reads -march={} without it",
                extension,
                isa);
        }
    }
    return result;
}

/// An option of GCC's as clang's driver takes it: GCC's two-core tuning
/// form, `-mcpu=cortex-a72.cortex-a53`, as the first core alone (clang
/// knows no such name, and the first core's architecture is the code's;
/// the features after the cores keep their dots, `-mcpu=cortex-m7+nofp.dp`),
/// and a RISC-V ISA string as clang can read it.
std::string for_clang(llvm::StringRef arg, const llvm::Triple& target) {
    if(arg.starts_with("-mcpu=")) {
        auto features = arg.find('+');
        auto first = (arg.substr(0, features).split('.').first + arg.substr(features)).str();
        if(first != arg) {
            LOG_INFO("clang reads {} as {}, its first core", arg, first);
        }
        return first;
    }
    llvm::StringRef isa = arg;
    if(target.isRISCV() && isa.consume_front("-march=")) {
        return "-march=" + riscv_isa_for_clang(isa);
    }
    return arg.str();
}

}  // namespace

GCCStyleOutput parse_gcc_style_output(llvm::StringRef out, llvm::StringRef err) {
    GCCStyleOutput output;
    output.clang_job = parse_clang_job(err);

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
            output.search_dirs.push_back({.path = dir.str(), .framework = framework});
        } else if(line.starts_with("#include <...> search starts here:")) {
            in_search_list = true;
        } else if(line.consume_front("Target: ")) {
            output.target = line.trim().str();
        } else if(line.consume_front("LIBRARY_PATH=")) {
            // A Cygwin or MSYS2 GCC lists POSIX paths, separated POSIX-style.
            char separator = is_posix_path(line) ? ':' : path::list_separator;
            llvm::SmallVector<llvm::StringRef> dirs;
            line.split(dirs, separator, -1, false);
            for(auto dir: dirs) {
                library_path.push_back(lexical((dir + "/include").str()));
            }
        } else if(auto job = gcc_job_line(line); job && output.clang_job.empty()) {
            output.gcc_job = job->first.str();
            // The options kept are joined forms: splitting at spaces cuts
            // none of them.
            llvm::SmallVector<llvm::StringRef> tokens;
            llvm::SplitString(job->second, tokens);
            for(auto token: tokens) {
                if(token.starts_with("-D") || token.starts_with("-U") || token.starts_with("-m") ||
                   token.starts_with("-f")) {
                    output.gcc_options.push_back(token.str());
                }
            }
        }
    }

    // The private directory holds GCC's builtin headers: it is the
    // `include` beside the libraries of GCC's own library directory, which
    // LIBRARY_PATH lists after any multilib variant (thumb/v7e-m+fp/hard/).
    for(std::size_t i = 0; i < output.search_dirs.size() && !output.private_dir; i += 1) {
        if(ranges::contains(library_path, lexical(output.search_dirs[i].path))) {
            output.private_dir = i;
        }
    }

    llvm::StringRef cplusplus;
    llvm::StringRef stdc_version;
    llvm::StringRef riscv_xlen;
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
        } else if(name == "__riscv_xlen") {
            riscv_xlen = value;
        } else if(name == "__STRICT_ANSI__") {
            strict = true;
        } else if(name == "__OPENCL_C_VERSION__" || name == "__OPENCL_CPP_VERSION__") {
            // OpenCL's version is -cl-std's; clang rejects any -std with it.
            opencl = true;
        }
    }
    if(!opencl) {
        output.standard = standard_from_macros(cplusplus, stdc_version, strict);
    }
    llvm::Triple target(output.target);
    if(target.getArch() == llvm::Triple::UnknownArch && target.getArchName() == "riscv" &&
       !riscv_xlen.empty()) {
        target.setArchName(("riscv" + riscv_xlen).str());
        LOG_INFO("GCC's target {} names no width: clang reads it as {}, as __riscv_xlen says",
                 output.target,
                 target.str());
        output.target = target.str();
    }
    return output;
}

kota::task<std::expected<GCCStyleOutput, std::string>>
    run_gcc_style(llvm::ArrayRef<std::string> args, std::size_t driver_tokens, std::string cwd) {
    std::vector<std::string> argv(args.begin(), args.begin() + driver_tokens);
    argv.insert(argv.end(), {"-E", "-dM", "-v"});
    argv.insert(argv.end(), args.begin() + driver_tokens, args.end());
    auto command = join_command(argv);
    auto output = co_await execute(std::move(argv), std::move(cwd));
    if(!output) {
        co_return std::unexpected(std::move(output.error()));
    }
    auto parsed = parse_gcc_style_output(output->out, output->err);
    if(parsed.clang_job.empty() && !output->failure.empty()) {
        // The version GCC prints last says nothing: its errors come before
        // it (an unknown option), or before "compilation terminated.".
        llvm::SmallVector<llvm::StringRef> lines;
        llvm::StringRef(output->err).split(lines, '\n', -1, false);
        std::string reported;
        for(auto line: lines) {
            line = line.trim();
            if(line.contains(" error: ")) {
                reported += reported.empty() ? "" : "; ";
                reported += line;
            }
        }
        if(reported.empty() && !lines.empty()) {
            reported = lines.back().trim();
        }
        auto error =
            reported.empty() ? output->failure : std::format("{}: {}", output->failure, reported);
#ifdef _WIN32
        // Windows looks for a program's DLLs on PATH: without its runtime's
        // directory there, a Cygwin or MSYS2 driver cannot start (exception
        // 0xC0000135), or cannot start its compiler proper.
        if(output->failure.ends_with("0xC0000135") ||
           output->err.contains("error while loading shared libraries")) {
            error +=
                " (a DLL it loads is missing: the bin directory of its runtime, Cygwin's or "
                "MSYS2's, belongs on clice's PATH, which clice.serverEnv sets in VS Code)";
        }
#endif
        LOG_WARN("The toolchain query {} failed: {}", command, error);
        co_return std::unexpected(std::move(error));
    }
    co_return co_await translate_posix_paths(std::move(parsed), args[0]);
}

std::expected<Job, std::string> query_as_gcc(const GCCStyleOutput& output,
                                             llvm::ArrayRef<std::string> args,
                                             llvm::StringRef cwd) {
    std::vector<std::string> clang_args = {args[0],
                                           "--target=" + output.target,
                                           "-nostdinc",
                                           "-resource-dir",
                                           resource_dir().str()};
    auto parse_options =
        kota::option::ParseOptions{.visibility = family_visibility(CompilerFamily::GCC)};
    llvm::Triple target(output.target);
    for(auto& parsed: option::table().parse(output.gcc_options, parse_options)) {
        if(parsed && parsed->id != option::OPT_UNKNOWN) {
            clang_args.push_back(for_clang(output.gcc_options[parsed->index], target));
        }
    }
    for(llvm::StringRef arg: args.drop_front()) {
        clang_args.push_back(for_clang(arg, target));
    }

    auto job = query_driver(clang_args, cwd);
    if(!job) {
        return job;
    }
    for(std::size_t i = 0; i < output.search_dirs.size(); i += 1) {
        auto& dir = output.search_dirs[i];
        if(output.private_dir == i) {
            job->insert(job->end(), {"-internal-isystem", path::join(resource_dir(), "include")});
            continue;
        }
        job->insert(job->end(),
                    {dir.framework ? "-internal-iframework" : "-internal-isystem", dir.path});
    }
    return job;
}

kota::task<std::expected<Job, std::string>> ask_gcc_style(std::vector<std::string> args,
                                                          std::size_t driver_tokens,
                                                          std::string cwd) {
    // A universal build (`-arch arm64 -arch x86_64`) compiles once for each
    // architecture, and a driver preprocesses for one only: clice reads the
    // code for the first.
    for(std::size_t i = 0, seen = 0; i + 1 < args.size();) {
        if(args[i] == "-arch") {
            seen += 1;
            if(seen > 1) {
                args.erase(args.begin() + i, args.begin() + i + 2);
                continue;
            }
        }
        i += 1;
    }

    auto output = co_await run_gcc_style(args, driver_tokens, cwd);
    if(!output) {
        co_return std::unexpected(std::move(output.error()));
    }
    auto standard =
        names_standard(args) || output->standard.empty() ? "" : "-std=" + output->standard;
    if(!output->clang_job.empty()) {
        auto job = std::move(output->clang_job);
        // As for cl: an MSVC target's default template deferral leaves
        // deferred bodies without an AST, so it stays only when the command
        // asks for it.
        if(!ranges::contains(args, "-fdelayed-template-parsing")) {
            std::erase(job, "-fdelayed-template-parsing");
        }
        if(!standard.empty()) {
            job.push_back(std::move(standard));
        }
        co_return job;
    }
    if(output->gcc_job.empty()) {
        co_return std::unexpected(std::format("{} runs no compiler job for the input", args[0]));
    }
    if(!standard.empty()) {
        args.push_back(std::move(standard));
    }
    co_return query_as_gcc(*output, args, cwd);
}

bool clang_warning(llvm::StringRef value) {
    auto group = [](llvm::StringRef name) {
        return clang::DiagnosticIDs::getGroupForWarningOption(name).has_value();
    };
    if(value == "format=0") {
        return true;
    }
    value.consume_front("no-");
    if(value == "system-headers" || value == "everything") {
        return true;
    }
    if(value.consume_front("error")) {
        return value.empty() || value == "-implicit-function-declaration" ||
               (value.consume_front("=") && (value.empty() || group(value)));
    }
    if(value.consume_front("fatal-errors")) {
        return value.empty() ||
               ((value.consume_front("=") || value.consume_front("-")) && group(value));
    }
    return group(value);
}

}  // namespace clice
