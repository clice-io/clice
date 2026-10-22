module;

#include "modules/prelude.h"

module clice;

import :command.toolchains.clang;
import :vfs.file_system;

namespace clice {

namespace {

/// The first error clang's driver reports, which says why it built no job
/// when it builds none; it builds one despite most (an unknown flag).
struct FirstError : clang::DiagnosticConsumer {
    std::string message;

    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& info) override {
        if(level >= clang::DiagnosticsEngine::Error && message.empty()) {
            llvm::SmallString<128> text;
            info.FormatDiagnostic(text);
            message = text.str();
        }
    }
};

}  // namespace

std::expected<Job, std::string> query_driver(llvm::ArrayRef<std::string> args,
                                             llvm::StringRef cwd) {
    clang::DiagnosticOptions options;
    FirstError first_error;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    options,
                                    &first_error,
                                    /*ShouldOwnClient=*/false);
    auto failure = [&](llvm::StringRef what) {
        return first_error.message.empty()
                   ? std::format("{} for {}", what.str(), args.front())
                   : std::format("{} for {}: {}", what.str(), args.front(), first_error.message);
    };

    llvm::SmallVector<const char*, 256> list;
    list.emplace_back(args.front().c_str());
    list.emplace_back("-fsyntax-only");
    for(auto& argument: args.drop_front()) {
        list.emplace_back(argument.c_str());
    }
    llvm::ArrayRef<const char*> arguments = list;

    /// Note that clang use the `DriverExecutable` to determine the driver mode when
    /// --driver-mode is not found in the arguments, and `TargetTriple` is used when
    /// non --target argument is found in the arguments list. See
    /// `clang::driver::BuildCompilation`. We use default arguments because we will
    /// inject related commands before querying.
    auto view = llvm::makeIntrusiveRefCnt<vfs::View>();
    if(!cwd.empty()) {
        [[maybe_unused]] auto error = view->setCurrentWorkingDirectory(cwd);
    }
    clang::driver::Driver driver(/*DriverExecutable=*/arguments[0],
                                 /*TargetTriple=*/llvm::sys::getDefaultTargetTriple(),
                                 /*Diags=*/engine,
                                 /*Title=*/"clang LLVM compiler",
                                 /*VFS=*/view);
    driver.setCheckInputsExist(false);
    driver.setProbePrecompiled(false);

    std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(arguments));
    if(!compilation) {
        return std::unexpected(failure("Failed to build compilation"));
    }

    // We expect to get back exactly one command job, if we didn't something
    // failed. Offload compilation is an exception as it creates multiple jobs. If
    // that's the case, we proceed with the first job. If caller needs a
    // particular job, it should be controlled via options (e.g.
    // --cuda-{host|device}-only for CUDA) passed to the driver.
    const clang::driver::JobList& jobs = compilation->getJobs();
    auto cmd = llvm::find_if(jobs, [](const clang::driver::Command& cmd) {
        return cmd.getCreator().getName() == llvm::StringRef("clang");
    });
    if(cmd == jobs.end()) {
        return std::unexpected(failure("No clang job found"));
    }

    Job job{arguments[0]};
    job.insert(job.end(), cmd->getArguments().begin(), cmd->getArguments().end());
    return job;
}

bool names_standard(llvm::ArrayRef<std::string> args) {
    return std::ranges::any_of(args, [](llvm::StringRef arg) { return arg.starts_with("-std="); });
}

std::expected<Job, std::string> ask_clang(llvm::ArrayRef<std::string> args,
                                          llvm::ArrayRef<std::string> mode,
                                          llvm::StringRef cwd) {
    // Before C++20, clang defers template bodies on MSVC targets so that
    // code written for MSVC's lookup rules compiles, but a deferred body has
    // no AST: every feature inside it goes blind. Placed before the
    // command's own flags, this one keeps that default out of the cc1
    // command and leaves deferral to a command asking for it
    // (`-fdelayed-template-parsing`, clang-cl's `/Zc:twoPhase-`), which
    // every compile of the file then follows.
    std::vector<std::string> clang_args = {args[0]};
    clang_args.insert(clang_args.end(), mode.begin(), mode.end());
    clang_args.push_back("-fno-delayed-template-parsing");
    clang_args.insert(clang_args.end(), args.begin() + 1, args.end());
    return query_driver(clang_args, cwd);
}

std::expected<Job, std::string> ask_clang_for(std::vector<std::string> args,
                                              std::size_t driver_tokens,
                                              llvm::StringRef cwd) {
    args.erase(args.begin() + 1, args.begin() + driver_tokens);
    std::vector<std::string> mode;
    auto name = clang::driver::ToolChain::getTargetAndModeFromProgramName(args[0]);
    // clang takes the prefix only when a backend it links knows it, and
    // clice links none: any prefix naming an architecture is a target.
    if(llvm::Triple(name.TargetPrefix).getArch() != llvm::Triple::UnknownArch) {
        mode.push_back("--target=" + name.TargetPrefix);
    }
    return ask_clang(args, mode, cwd);
}

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
                             {202302, "23"},
                             {202400, "26"}
        },
                         "2d");
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
