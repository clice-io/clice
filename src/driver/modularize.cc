module;

#include "modules/prelude.h"

module clice;

import :analysis.module_graph;
import :analysis.rewriting;
import :analysis.wrapping;
import :driver.driver;
import :vfs.file_system;

namespace clice::driver {

namespace {

/// What the build needs: the wrapped modules, and the program modules the
/// partition rewrites.
struct Plan {
    analysis::Wrapping::Plan wrapping;
    analysis::Rewriting::Plan rewriting;
};

/// Where a run writes, and whether it writes mirrors.
struct OutputOptions {
    DecoKV(help = "Directory to write the modules, macro headers, mirrors and prelude into",
           required = false)
    <std::string> out;

    DecoFlag(names = {"--no-mirrors"},
             help =
                 "Write no mirrors: what still includes a wrapped header parses it beside "
                 "the import",
             required = false)
    no_mirrors;
};

struct ModularizeOptions {
    kota::deco::decl::HelpOption help;

    DecoKV(help =
               "JSON file assigning modules by globs, first match wins; a module is wrapped, "
               R"("textual": true stays headers, "external": true is std, "provides": )"
               R"("std.compat" names who exports its names, "rewrite": true rewrites a )"
               R"(program module into module units, its primary interface at "primary")",
           required = false)
    <std::string> partition;

    DecoKV(help =
               "A glob over workspace-relative paths, absolute ones outside the workspace: "
               "the files the partition and the program span",
           required = false)
    <std::vector<std::string>> scope;

    DecoKV(help =
               "libc++'s module sources (share/libc++/v1): the partition's std module is "
               "imported as std.compat",
           required = false)
    <std::string> std;

    OutputOptions output;
    WorkspaceOptions where;
    LogLevelOption log;
};

struct MergeOptions {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<FILE>",
              help =
                  "JSON file listing the build configurations to merge, each a name, the "
                  "preprocessor condition holding in its compilations alone and the --out "
                  "directory a run for it wrote",
              required = false)
    <std::vector<std::string>> file;

    OutputOptions output;
    LogLevelOption log;
};

struct MergeFile {
    struct Configuration {
        std::string name;
        std::string condition;
        /// Relative to the merge file unless absolute.
        std::string out;
    };

    std::vector<Configuration> configurations;
};

llvm::SmallString<256> join_path(llvm::StringRef base, llvm::StringRef relative) {
    llvm::SmallString<256> path(base);
    llvm::sys::path::append(path, llvm::sys::path::Style::posix, relative);
    return path;
}

/// The paths a run's manifest lists. A hand-edited one reaches nothing
/// outside its directory.
llvm::SmallVector<llvm::StringRef> manifest_paths(llvm::StringRef manifest) {
    llvm::SmallVector<llvm::StringRef> paths;
    manifest.split(paths, '\n', -1, false);
    paths.erase(std::ranges::remove_if(paths,
                                       [](llvm::StringRef path) {
                                           return path.starts_with("/") || path.contains("..");
                                       })
                    .begin(),
                paths.end());
    return paths;
}

/// Write the files whose content changed, so a regeneration rebuilds only
/// what it touched, and remove the files the previous run wrote that this
/// one no longer produces: a stale empty header in a mirror would hide the
/// real one. The manifest `.modularize` lists what a run wrote; nothing else
/// under `out` is touched.
std::expected<void, std::string> write_files(llvm::StringRef out,
                                             llvm::ArrayRef<analysis::Wrapping::File> files) {
    auto at = [&](llvm::StringRef relative) {
        return join_path(out, relative);
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
    if(auto previous = vfs::read(at(".modularize"))) {
        for(auto relative: manifest_paths((*previous)->getBuffer())) {
            if(written.contains(relative)) {
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
    if(auto error = vfs::write(at(".modularize"), manifest)) {
        return std::unexpected(
            std::format("cannot write {}/.modularize: {}", out.str(), error.message()));
    }
    return {};
}

std::expected<std::vector<analysis::Configuration>, std::string>
    read_configurations(llvm::StringRef path) {
    auto buffer = vfs::read(path);
    if(!buffer) {
        return std::unexpected(
            std::format("cannot read {}: {}", path.str(), buffer.error().message()));
    }
    MergeFile file;
    if(auto result = kota::codec::json::from_string((*buffer)->getBuffer(), file); !result) {
        return std::unexpected(
            std::format("{} is not a merge file: {}", path.str(), result.error().message));
    }
    std::vector<analysis::Configuration> configurations;
    for(auto& entry: file.configurations) {
        auto out = llvm::sys::path::is_absolute(entry.out)
                       ? entry.out
                       : join_path(llvm::sys::path::parent_path(path), entry.out).str().str();
        auto manifest = vfs::read(join_path(out, ".modularize"));
        if(!manifest) {
            return std::unexpected(
                std::format("configuration {}: {} holds no modularize output", entry.name, out));
        }
        auto& configuration = configurations.emplace_back(analysis::Configuration{
            .name = std::move(entry.name),
            .condition = std::move(entry.condition),
        });
        for(auto relative: manifest_paths((*manifest)->getBuffer())) {
            auto content = vfs::read(join_path(out, relative));
            if(!content) {
                return std::unexpected(std::format("cannot read {}/{}: {}",
                                                   out,
                                                   relative.str(),
                                                   content.error().message()));
            }
            configuration.files.push_back({relative.str(), (*content)->getBuffer().str()});
        }
    }
    return configurations;
}

int run_merge(const MergeOptions& opts) {
    auto args = positional(opts.file.as_optional(), {"<FILE>"});
    if(!args) {
        return usage_error(Errors::JSON, args.error());
    }
    if(!opts.output.out) {
        return fail("modularize merge needs --out");
    }
    auto configurations = read_configurations(args->front());
    if(!configurations) {
        return fail(configurations.error());
    }
    auto merged = analysis::merge(*configurations, !opts.output.no_mirrors);
    if(!merged) {
        return fail(merged.error());
    }
    if(auto written = write_files(Spelling(*opts.output.out, Spelling::cwd()).str(), merged->files);
       !written) {
        return fail(written.error());
    }
    print_json(Plan{.wrapping = std::move(merged->plan)});
    return 0;
}

int run_modularize(const ModularizeOptions& opts) {
    if(!opts.partition || !opts.output.out) {
        return fail("modularize needs --partition and --out");
    }
    auto libcxx = analysis::read_std_modules(opts.std.value_or(""));
    if(!libcxx) {
        return fail(libcxx.error());
    }
    auto spec = analysis::read_partition({}, *opts.partition, *libcxx);
    if(!spec) {
        return fail(spec.error());
    }
    auto loaded = analysis::load_facts(opts.where.root(),
                                       opts.where.configuration.tag(),
                                       opts.scope.value_or(std::vector<std::string>{}));
    if(!loaded) {
        print_json(Failure{.error = std::move(loaded.error().message),
                           .stale = std::move(loaded.error().dropped)});
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
    auto wrapping =
        analysis::wrap(*partition, *interfaces, *libcxx, loaded->root, !opts.output.no_mirrors);
    if(!wrapping) {
        return fail(wrapping.error());
    }

    Spelling out_spelling(*opts.output.out, Spelling::cwd());
    auto out = out_spelling.str();
    std::optional<analysis::Rewriting> rewriting;
    if(std::ranges::any_of(partition->primaries, [](auto& primary) { return !primary.empty(); })) {
        // The prelude by its path from the workspace root, which the
        // rewritten files' include path holds.
        CanonicalPath canonical_out(out_spelling);
        llvm::StringRef prelude_dir = canonical_out;
        auto inside = prelude_dir.consume_front(loaded->root) && prelude_dir.consume_front("/");
        llvm::SmallString<256> prelude(inside ? prelude_dir : llvm::StringRef(out));
        llvm::sys::path::append(prelude, llvm::sys::path::Style::posix, wrapping->plan.prelude);
        auto rewritten =
            analysis::rewrite(loaded->facts, *partition, *interfaces, prelude, loaded->root);
        if(!rewritten) {
            return fail(rewritten.error());
        }
        rewriting = std::move(*rewritten);
    }

    if(auto written = write_files(out, wrapping->files); !written) {
        return fail(written.error());
    }
    Plan plan{.wrapping = std::move(wrapping->plan)};
    if(rewriting) {
        for(auto& file: rewriting->files) {
            auto path = join_path(loaded->root, file.path);
            if(auto error = vfs::create_directories(llvm::sys::path::parent_path(path))) {
                return fail(std::format("cannot create the directory of {}: {}",
                                        path.str().str(),
                                        error.message()));
            }
            if(auto error = vfs::write(path, file.content)) {
                return fail(std::format("cannot write {}: {}", path.str().str(), error.message()));
            }
        }
        for(auto& removed: rewriting->plan.removed) {
            auto path = join_path(loaded->root, removed);
            if(auto error = vfs::remove(path)) {
                return fail(std::format("cannot remove {}: {}", path.str().str(), error.message()));
            }
        }
        plan.rewriting = std::move(rewriting->plan);
    }
    print_json(plan);
    return 0;
}

CommandHandler build(const char*) {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "merge",
        .description = "Merge the wrappings of several build configurations",
        .run = operation<MergeOptions>("clice modularize merge <FILE> [OPTIONS]",
                                       "modularize",
                                       Errors::JSON,
                                       run_merge),
    });
    return with_operations("",
                           std::move(operations),
                           Errors::JSON,
                           operation<ModularizeOptions>("clice modularize [OPTIONS]",
                                                        "modularize",
                                                        Errors::JSON,
                                                        run_modularize));
}

[[maybe_unused]] const bool registered = register_command(
    Command::modularize,
    {.description = "Wrap a partition's libraries as modules over their headers, rewrite its "
                    "program modules into module units, or merge the wrappings of several build "
                    "configurations",
     .build = build});

}  // namespace

}  // namespace clice::driver
