module clice;

import :command.argument;
import :command.table;
import :command.toolchains.cl;
import :command.toolchains.clang;
import :support.process;
import :vfs.path;

namespace clice {

CLOutput parse_cl_output(llvm::StringRef out) {
    CLOutput output;
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
            output.standard = standard_from_macros(value, "", true);
        } else if(label == "msc_full_ver") {
            output.version = std::format("{}.{}.{}",
                                         number / 10'000'000,
                                         number / 100'000 % 100,
                                         number % 100'000);
        }
    }
    return output;
}

std::vector<std::string> cl_driver_mode(llvm::StringRef driver, CompilerFamily family) {
    std::vector<std::string> mode = {"--driver-mode=cl"};
    if(family != CompilerFamily::MSVC || env_string("INCLUDE")) {
        return mode;
    }
    auto host = path::parent(path::parent(driver));
    auto bin = path::parent(host);
    if(llvm::StringRef(path::filename(host)).starts_with_insensitive("host") &&
       llvm::StringRef(path::filename(bin)).equals_insensitive("bin")) {
        mode.insert(mode.end(), {"/vctoolsdir", std::string(path::parent(bin))});
    }
    return mode;
}

std::string visual_studio_bin(llvm::StringRef program) {
#ifdef _WIN32
    bool cl = program.equals_insensitive("cl");
    if(!cl && !program.equals_insensitive("clang-cl")) {
        return {};
    }
    // LIMITATION: the newest toolset, even when the build pinned an older
    // one (-vcvars_ver) and several are installed: _MSC_VER and the STL
    // headers follow the newest.
    std::string toolchain;
    llvm::ToolsetLayout layout;
    if(!llvm::findVCToolChainViaSetupConfig(*llvm::vfs::getRealFileSystem(),
                                            std::nullopt,
                                            toolchain,
                                            layout)) {
        return {};
    }
    auto arch = llvm::Triple(llvm::sys::getProcessTriple()).getArch();
    if(cl) {
        return llvm::getSubDirectoryPath(llvm::SubDirectoryType::Bin, layout, toolchain, arch);
    }
    // <VS>/VC/Tools/MSVC/<version> is the toolset, <VS>/VC/Tools/Llvm/<arch>/bin
    // the clang-cl beside it.
    auto tools = path::parent(path::parent(toolchain));
    return path::join(tools, "Llvm", arch == llvm::Triple::aarch64 ? "ARM64" : "x64", "bin");
#else
    return {};
#endif
}

std::vector<std::string> cl_question(llvm::ArrayRef<std::string> args, const Command& command) {
    // cl hears only the command's `/std:` (and the driver mode that makes
    // a `clang` clang-cl): its other flags are spelled for clang's cl mode
    // (`/WX` arrives as -Werror, which cl rejects), and a standard or
    // version set in clang's spelling (a rule's `-std=c++23`) is clang's
    // to read, not cl's.
    std::vector<std::string> question = {args[0], "/nologo", "/EP"};
    // clang-cl shows the job it runs, which names its resource directory:
    // clang's own driver would take one of its own version beside it.
    if(command.family == CompilerFamily::ClangCL) {
        question.push_back("-v");
    }
    for(auto& arg: command.args) {
        if(arg.opt_id == clang_options::OPT__SLASH_std ||
           arg.opt_id == clang_options::OPT_driver_mode) {
            render_driver_arg(arg, command.family, [&](std::string_view part) {
                question.emplace_back(part);
            });
        }
    }
    return question;
}

std::expected<Job, std::string> read_cl_answer(llvm::ArrayRef<std::string> args,
                                               const Command& command,
                                               const ProgramOutput& answer,
                                               llvm::StringRef cwd) {
    auto family = command.family;
    if(!answer.failure.empty()) {
        // cl reports its errors on stdout, clang-cl on stderr.
        auto last_line = llvm::StringRef(answer.err.empty() ? answer.out : answer.err).rtrim();
        last_line = last_line.substr(last_line.rfind('\n') + 1);
        return std::unexpected(std::format("{}: {}", answer.failure, last_line.str()));
    }
    auto parsed = parse_cl_output(answer.out);

    auto mode = cl_driver_mode(args[0], family);
    if(!reads_clang_config(family)) {
        mode.push_back(no_clang_config.str());
    }
    auto clang_job = parse_clang_job(answer.err);
    if(auto it = ranges::find(clang_job, "-resource-dir");
       it != clang_job.end() && std::next(it) != clang_job.end()) {
        mode.insert(mode.end(), {"-resource-dir", *std::next(it)});
    }
    std::vector<std::string> flags = {args[0]};
    auto taken = flags_for_clang(args.drop_front(), family);
    flags.insert(flags.end(), taken.begin(), taken.end());
    auto job = ask_clang(flags, mode, cwd);
    if(!job) {
        return job;
    }
    if(!names_standard(command.args) && !parsed.standard.empty()) {
        job->args.push_back("-std=" + parsed.standard);
    }
    bool names_version = ranges::any_of(command.args, [](const auto& arg) {
        auto id = effective_option(arg);
        return id == clang_options::OPT_fms_compatibility_version ||
               id == clang_options::OPT_fmsc_version;
    });
    if(family == CompilerFamily::MSVC && !names_version && !parsed.version.empty()) {
        job->args.push_back("-fms-compatibility-version=" + parsed.version);
    }
    return job;
}

}  // namespace clice
