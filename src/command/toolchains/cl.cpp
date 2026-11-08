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

async::task<std::expected<Job, std::string>> ask_cl(llvm::ArrayRef<std::string> args,
                                                    const Command& command,
                                                    llvm::StringRef input,
                                                    std::string cwd) {
    auto family = command.family;
    if(auto error = fs::write_file(input, cl_query_source)) {
        co_return std::unexpected(
            std::format("Failed to write {}: {}", input.str(), error.message()));
    }
    // cl hears only the command's `/std:` (and the driver mode that makes
    // a `clang` clang-cl): its other flags are spelled for clang's cl mode
    // (`/WX` arrives as -Werror, which cl rejects), and a standard or
    // version set in clang's spelling (a rule's `-std=c++23`) is clang's
    // to read, not cl's.
    std::vector<std::string> query = {args[0], "/nologo", "/EP"};
    // clang-cl shows the job it runs, which names its resource directory:
    // clang's own driver would take one of its own version beside it.
    if(family == CompilerFamily::ClangCL) {
        query.push_back("-v");
    }
    bool names_version = false;
    for(auto& arg: command.args) {
        if(arg.opt_id == clang_options::OPT__SLASH_std ||
           arg.opt_id == clang_options::OPT_driver_mode) {
            render_driver_arg(arg, family, [&](std::string_view part) {
                query.emplace_back(part);
            });
        }
        auto id = effective_option(arg);
        names_version = names_version || id == clang_options::OPT_fms_compatibility_version ||
                        id == clang_options::OPT_fmsc_version;
    }
    query.push_back(input.str());
    auto output = co_await execute(std::move(query), cwd);
    if(!output) {
        co_return std::unexpected(std::move(output.error()));
    }
    if(!output->failure.empty()) {
        // cl reports its errors on stdout, clang-cl on stderr.
        auto last_line = llvm::StringRef(output->err.empty() ? output->out : output->err).rtrim();
        last_line = last_line.substr(last_line.rfind('\n') + 1);
        co_return std::unexpected(std::format("{}: {}", output->failure, last_line.str()));
    }
    auto parsed = parse_cl_output(output->out);

    auto mode = cl_driver_mode(args[0], family);
    auto clang_job = parse_clang_job(output->err);
    if(auto it = ranges::find(clang_job, "-resource-dir");
       it != clang_job.end() && std::next(it) != clang_job.end()) {
        mode.insert(mode.end(), {"-resource-dir", *std::next(it)});
    }
    std::vector<std::string> flags = {args[0]};
    auto taken = flags_for_clang(args.drop_front(), family);
    flags.insert(flags.end(), taken.begin(), taken.end());
    auto job = ask_clang(flags, mode, cwd);
    if(!job) {
        co_return job;
    }
    if(!names_standard(command.args) && !parsed.standard.empty()) {
        job->args.push_back("-std=" + parsed.standard);
    }
    if(family == CompilerFamily::MSVC && !names_version && !parsed.version.empty()) {
        job->args.push_back("-fms-compatibility-version=" + parsed.version);
    }
    co_return job;
}

}  // namespace clice
