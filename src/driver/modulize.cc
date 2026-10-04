#include <format>
#include <optional>
#include <string>
#include <vector>

#include "analysis/module_graph.h"
#include "analysis/wrapping.h"
#include "driver/analysis_support.h"
#include "driver/driver.h"
#include "driver/query_support.h"
#include "vfs/file_system.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct ModulizeOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Build configuration to read, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Comma-separated globs over workspace-relative paths, absolute ones outside "
               "the workspace: the files the partition and the program span",
           required = false)
    <std::string> scope;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "JSON file assigning modules by globs, first match wins; a module is wrapped, "
               R"("textual": true stays headers, "external": true is std, "provides": )"
               R"("std.compat" names who exports its names)",
           required = false)
    <std::string> partition;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "libc++'s module sources (share/libc++/v1): the partition's std module is "
               "imported as std.compat",
           required = false)
    <std::string> std;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Directory to write the modules, macro headers, mirrors and prelude into",
           required = false)
    <std::string> out;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off (default: warn)",
           required = false)
    <std::string> log_level;
};

auto make_modulize_command() {
    return kota::deco::cli::command<ModulizeOptions>("clice modulize [OPTIONS]");
}

/// Write the files whose content changed, so a regeneration rebuilds only
/// what it touched, and remove the files the previous run wrote that this
/// one no longer produces: a stale empty header in a mirror would hide the
/// real one. The manifest `.modulize` lists what a run wrote; nothing else
/// under `out` is touched.
std::expected<void, std::string> write_files(llvm::StringRef out,
                                             llvm::ArrayRef<analysis::Wrapping::File> files) {
    auto at = [&](llvm::StringRef relative) {
        llvm::SmallString<256> path(out);
        llvm::sys::path::append(path, llvm::sys::path::Style::posix, relative);
        return path;
    };
    llvm::StringSet<> written;
    std::string manifest;
    for(auto& file: files) {
        written.insert(file.path);
        manifest += file.path + "\n";
        auto path = at(file.path);
        if(auto existing = vfs::read(path);
           existing && (*existing)->getBuffer() == llvm::StringRef(file.content)) {
            continue;
        }
        if(auto error = vfs::create_directories(llvm::sys::path::parent_path(path))) {
            return std::unexpected(std::format("cannot create the directory of {}: {}",
                                               path.str().str(),
                                               error.message()));
        }
        if(auto error = vfs::write(path, file.content)) {
            return std::unexpected(
                std::format("cannot write {}: {}", path.str().str(), error.message()));
        }
    }
    if(auto previous = vfs::read(at(".modulize"))) {
        llvm::SmallVector<llvm::StringRef> lines;
        (*previous)->getBuffer().split(lines, '\n', -1, false);
        for(auto relative: lines) {
            // A hand-edited manifest reaches nothing outside `out`.
            if(written.contains(relative) || relative.starts_with("/") || relative.contains("..")) {
                continue;
            }
            if(auto error = vfs::remove(at(relative))) {
                return std::unexpected(std::format("cannot remove {}/{}: {}",
                                                   out.str(),
                                                   relative.str(),
                                                   error.message()));
            }
        }
    }
    if(auto error = vfs::write(at(".modulize"), manifest)) {
        return std::unexpected(
            std::format("cannot write {}/.modulize: {}", out.str(), error.message()));
    }
    return {};
}

int run_modulize(const ModulizeOptions& opts) {
    auto fail = [](std::string error) {
        print_json(Failure{.error = std::move(error)});
        return 1;
    };

    if(!opts.partition || !opts.out) {
        return fail("modulize needs --partition and --out");
    }
    auto libcxx = read_std(opts.std.value_or(""));
    if(!libcxx) {
        return fail(libcxx.error());
    }
    auto spec = read_partition({}, *opts.partition, *libcxx);
    if(!spec) {
        return fail(spec.error());
    }
    auto loaded = load_facts(opts.workspace.value_or(""),
                             opts.configuration.value_or(""),
                             opts.scope.value_or(""));
    if(!loaded) {
        print_json(loaded.error());
        return 1;
    }
    auto partition = analysis::partition(loaded->facts, *spec);
    if(!partition) {
        return fail(partition.error());
    }
    analysis::Annotations annotations;
    analysis::Report report{.facts = loaded->facts,
                            .partition = *partition,
                            .annotations = annotations};
    auto interfaces = report.interface("");
    if(!interfaces) {
        return fail(interfaces.error());
    }
    auto wrapping = analysis::wrap(*partition, *interfaces, *libcxx, loaded->root);
    if(!wrapping) {
        return fail(wrapping.error());
    }

    auto out = Spelling(*opts.out, Spelling::cwd()).str();
    if(auto written = write_files(out, wrapping->files); !written) {
        return fail(written.error());
    }
    print_json(wrapping->plan);
    return 0;
}

}  // namespace

void add_modulize(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto command = make_modulize_command();
    command
        .matchAll([&exit_code](ModulizeOptions opts) {
            if(opts.help) {
                auto help = make_modulize_command();
                print_usage(help);
                exit_code = 0;
                return;
            }
            if(!apply_log_level(opts.log_level.value_or("warn")))
                return;
            logging::stderr_logger("modulize", logging::options);
            exit_code = run_modulize(opts);
        })
        .on_error([](auto err) { print_json(Failure{.error = err.message}); });
    root.add({.name = "modulize",
              .description = "Wrap a partition's libraries as modules over their headers"},
             std::move(command));
}

}  // namespace clice::driver
