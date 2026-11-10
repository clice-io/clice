module clice;

import :command.database;
import :command.file_kind;
import :driver.driver;
import :driver.run;
import :feature.formatting;
import :project.discovery;
import :project.project;
import :support.logging;
import :support.text;
import :vfs.file_system;
import :vfs.path;

namespace clice::driver {

namespace {

KANNOTATE_TYPE(struct Format, description = "Format C++ source files");
struct Format {
    KANNOTATE(&paths,
              cli::positional = true,
              cli::meta_var = "<PATH>",
              description =
                  "Files to format, or directories to format the build's own files under; "
                  "relative to the current directory (default: the build's own files)")
    std::vector<std::string> paths;

    KANNOTATE(&check,
              description = "Report the files that need formatting instead of rewriting them")
    bool check = false;

    ProjectOptions where;
    JobsOption jobs;
    LogLevelOption log;
};

struct Request {
    /// The project's root as the command line spells it.
    Spelling root;

    /// Its settings (load_settings), without errors.
    LoadedSettings loaded;

    /// What to format instead of the build's own files, canonical and
    /// absolute: a directory narrows the set to the files under it, a
    /// file is formatted whether or not the build knows it.
    std::vector<Spelling> paths;

    /// Files formatted at once, each on a thread; 0 takes the hardware
    /// concurrency.
    std::uint32_t jobs = 0;

    /// Report the files formatting would change instead of rewriting
    /// them.
    bool check = false;
};

struct Result {
    /// 0 = every file formatted (or, with --check, nothing to change),
    /// 1 = --check found files to format, 2 = the run could not complete:
    /// a file's style could not be read, a file could not be read or
    /// written.
    int exit_code = 0;

    /// Files formatted or checked.
    std::size_t files = 0;

    /// --check: the files formatting would change, sorted.
    std::vector<std::string> unformatted;

    /// --check: where each of them needs formatting, for the user.
    std::string output;

    /// Why the run could not complete, for the user; empty otherwise.
    std::string error;

    double seconds = 0;
};

/// What clang-format can format: the C family clang derives types from,
/// and `.cuh`, which clang does not list but CUDA code uses.
bool formats(llvm::StringRef path) {
    return clang::driver::types::isDerivedFromC(suffix_type(path)) ||
           path::extension(path) == ".cuh";
}

/// A file to format: read and rewritten where its bytes are, which keeps a
/// symlink a link, and formatted under the name the build or the command
/// line gives it, which picks its style as an open document's name does.
struct Target {
    CanonicalPath path;
    Spelling name;
};

/// The files `clice format` formats without arguments: the build's
/// translation units that are the project's own (project_file: not in
/// a build tree, where FetchContent puts the sources it fetches) and
/// every file they include that is the project's own — inside it,
/// outside the cache directory, the build trees (the directories below the
/// root holding one of the build's databases: what a build generates or
/// fetches lives there) and the commands' system include directories, of
/// a type clang-format formats, no matching rule saying `format = false`;
/// none in a project nested in the root (nested_project).
std::vector<Target> project_files(Project& project, llvm::ArrayRef<Fid> members) {
    auto& files = project.file_table;

    CanonicalRef root = project.root;
    auto cache_dir = project.settings.project.cache_identity;
    std::vector<CanonicalPath> skipped_dirs;
    auto skip = [&](CanonicalPath dir) {
        if(!ranges::contains(skipped_dirs, dir)) {
            skipped_dirs.push_back(std::move(dir));
        }
    };
    if(!cache_dir.empty()) {
        skip(cache_dir);
    }
    // A database at the project root, or above it, is a copy of the
    // build's or the build of a larger tree, not a build tree of its own;
    // a root link to build/compile_commands.json names build/.
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        CanonicalPath database(Spelling::absolute(project.cdb.source_path(SourceID(i))));
        auto directory = CanonicalRef(database).parent();
        if(directory != root && path::is_within(directory, root)) {
            skip(std::move(directory));
        }
    }
    for(auto& ref: project.scan_commands(members)) {
        auto search = project.toolchain.search_config(ref);
        for(auto& dir: llvm::ArrayRef(search.dirs).drop_front(search.system_start_idx)) {
            skip(CanonicalPath(Spelling::absolute(dir.path)));
        }
    }

    llvm::StringMap<bool> nested;
    auto in_nested = [&](CanonicalRef path) {
        bool found = false;
        path::walk_ancestors(path.parent(), [&](CanonicalRef dir) {
            if(dir == root || !path::is_within(dir, root)) {
                return false;
            }
            auto [it, inserted] = nested.try_emplace(llvm::StringRef(dir));
            if(inserted) {
                it->second = has_configuration_file(Spelling(dir));
            }
            found = it->second;
            return !found;
        });
        return found;
    };

    // A unit beside a database of its own directory, or in a directory its
    // own command searches as a system one, is still the project's: the
    // directories keep out only what units include.
    std::vector<Target> result;
    llvm::DenseSet<Fid> unit(members.begin(), members.end());
    for(auto fid: project.dep_graph.all_files()) {
        auto path = files.resolve(fid);
        // A header only a command forces in, which no unit's text
        // includes, is left as it is: often a build generated it (CMake's
        // cmake_pch.hxx).
        if(!formats(path) || !project.rules.formattable(path) || in_nested(path) ||
           (unit.contains(fid) ? !project_file(root, cache_dir, path)
                               : project.dep_graph.get_includers(fid).empty() ||
                                     ranges::any_of(skipped_dirs, [&](auto& dir) {
                                         return path::is_within(path, dir);
                                     }))) {
            continue;
        }
        result.push_back({.path = CanonicalPath(path), .name = files.spelling(fid)});
    }
    return result;
}

/// What formatting one file came to.
struct FileResult {
    /// --check: where the edits fall, as `clang-format --dry-run --Werror`
    /// reports them; empty for a formatted file.
    std::string violations;

    /// Why the file could not be formatted; empty otherwise.
    std::string error;
};

std::string violations(const Target& file,
                       llvm::StringRef content,
                       llvm::ArrayRef<TextReplacement> edits) {
    llvm::SourceMgr sources;
    sources.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBuffer(content, file.path, false),
                               llvm::SMLoc());
    std::string report;
    llvm::raw_string_ostream os(report);
    for(auto& edit: edits) {
        sources
            .GetMessage(llvm::SMLoc::getFromPointer(content.data() + edit.range.begin),
                        llvm::SourceMgr::DK_Error,
                        "code should be clang-formatted [-Wclang-format-violations]")
            .print(nullptr, os, /*ShowColors=*/false);
    }
    return report;
}

FileResult format_file(const Target& file, bool check) {
    auto content = vfs::read_bytes(file.path);
    if(!content) {
        return {.error = std::format("{}", content.error())};
    }
    llvm::StringRef text = *content;
    auto edits = feature::document_format(file.name, text);
    if(!edits) {
        return {.error = std::move(edits.error())};
    }
    if(check) {
        return {.violations = violations(file, text, *edits)};
    }
    if(edits->empty()) {
        return {};
    }
    std::string formatted;
    std::uint32_t offset = 0;
    for(auto& edit: *edits) {
        formatted += text.slice(offset, edit.range.begin);
        formatted += edit.text;
        offset = edit.range.end;
    }
    formatted += text.substr(offset);
    if(auto written = vfs::write_file(file.path, formatted); !written) {
        return {.error = std::format("{}", written.error())};
    }
    return {};
}

/// Format the project as the editor formats a document: load the build,
/// take its own files (the translation units and the project's files they
/// include, outside the build trees and the commands' system include
/// directories, no rule saying `format = false`), and format them in
/// process, `jobs` at a time.
Result format_project(const Request& options) {
    Timer timer;
    Result result;

    // Explicit files are taken as given; explicit directories narrow the
    // build's own files to those under them, and only they need the build.
    bool whole = options.paths.empty() ||
                 ranges::any_of(options.paths, [](auto& path) { return fs::is_directory(path); });
    auto opened_project = open_at(
        options.root,
        options.loaded,
        {.load = whole ? Project::OpenOptions::Load::Members : Project::OpenOptions::Load::Rules});
    auto& project = opened_project->project;
    auto& opened = opened_project->opened;

    std::vector<Target> files;
    std::vector<CanonicalPath> directories;
    CanonicalRef root = project.root;
    for(auto& path: options.paths) {
        if(fs::is_directory(path)) {
            directories.push_back(CanonicalPath(path));
        } else if(!fs::exists(path)) {
            result.exit_code = 2;
            result.error = std::format("{}: no such file", path);
            return result;
        } else if(!formats(path)) {
            result.exit_code = 2;
            result.error = std::format("{}: not a C-family source file", path);
            return result;
        } else {
            // Rewritten where its bytes are, which have to be the
            // project's too.
            auto target = CanonicalPath(path);
            if(!path::is_within(target, root)) {
                if(path::is_within(CanonicalPath(path.parent()), root)) {
                    result.exit_code = 2;
                    result.error = std::format("{}: links outside the project", path);
                    return result;
                }
            } else if(project.rules.formattable(target)) {
                files.push_back({.path = std::move(target), .name = path});
            }
        }
    }
    if(whole) {
        // A database that cannot be loaded makes the file set incomplete.
        if(project.cdb.source_count() == 0) {
            result.exit_code = 2;
            result.error =
                std::format("Nothing to format: {}", project.why_no_members(options.root));
            return result;
        }
        for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
            if(!project.cdb.loaded(SourceID(i))) {
                result.exit_code = 2;
                result.error = std::format("{}: the compilation database could not be loaded",
                                           project.cdb.source_path(SourceID(i)));
                return result;
            }
        }
        for(auto& file: project_files(project, opened.members)) {
            if(!directories.empty() && ranges::none_of(directories, [&](auto& directory) {
                   return path::is_within(file.path, directory);
               })) {
                continue;
            }
            // A symlink into the project is the project's; one pointing
            // out of it is not.
            if(path::is_within(file.path, root)) {
                files.push_back(std::move(file));
            }
        }
    }
    // A file named and also under a named directory keeps the name given.
    ranges::stable_sort(files, {}, &Target::path);
    auto duplicates = ranges::unique(files, {}, &Target::path);
    files.erase(duplicates.begin(), duplicates.end());
    result.files = files.size();
    if(files.empty()) {
        result.seconds = timer.ms() / 1000.0;
        return result;
    }

    std::uint32_t jobs = options.jobs != 0 ? options.jobs : sys::parallelism();
    std::vector<FileResult> formats(files.size());
    std::atomic<std::size_t> next = 0;
    std::vector<sys::Thread> threads;
    for(std::uint32_t i = 0; i < jobs && i < files.size(); i += 1) {
        // As much stack as the editor's formatting gets on a worker's pool
        // thread.
        auto thread = sys::Thread::start(8u << 20, [&] {
            for(auto index = next.fetch_add(1); index < files.size(); index = next.fetch_add(1)) {
                formats[index] = format_file(files[index], options.check);
            }
        });
        if(!thread) {
            LOG_FATAL("Cannot start a formatting thread: {}", thread.error());
        }
        threads.push_back(std::move(*thread));
    }
    for(auto& thread: threads) {
        thread.join();
    }

    llvm::SetVector<llvm::StringRef> errors;
    assert(ranges::size(files) == ranges::size(formats));
    for(auto [file, format]: views::zip(files, formats)) {
        result.output += format.violations;
        if(!format.error.empty()) {
            errors.insert(format.error);
        } else if(!format.violations.empty()) {
            result.unformatted.push_back(file.path.str());
        }
    }
    if(!errors.empty()) {
        result.exit_code = 2;
        result.error = llvm::join(errors, "\n");
    } else if(!result.unformatted.empty()) {
        result.exit_code = 1;
    }
    result.seconds = timer.ms() / 1000.0;
    return result;
}

int run_format(const Request& options) {
    auto result = format_project(options);
    write_output(stderr, result.output);
    if(result.exit_code == 2) {
        driver::println(stderr, "{}", result.error);
        return 2;
    }
    if(result.files == 0) {
        driver::println("No files to format under {}.", options.root);
        return 0;
    }
    if(!options.check) {
        driver::println("Formatted {} in {:.1f}s.",
                        counted(result.files, "file", "files"),
                        result.seconds);
    } else if(result.unformatted.empty()) {
        driver::println("Checked {} in {:.1f}s: all formatted.",
                        counted(result.files, "file", "files"),
                        result.seconds);
    } else {
        driver::println("Checked {} in {:.1f}s: {} need{} formatting.",
                        counted(result.files, "file", "files"),
                        result.seconds,
                        result.unformatted.size(),
                        result.unformatted.size() == 1 ? "s" : "");
    }
    return result.exit_code;
}

int run(Format& format) {
    auto read = read_project(format.where);
    if(!read) {
        return print_error(read.error());
    }
    auto& [root, loaded] = *read;
    Request request{
        .root = std::move(root),
        .loaded = std::move(loaded),
        .jobs = format.jobs.jobs,
        .check = format.check,
    };
    for(auto& argument: format.paths) {
        request.paths.emplace_back(argument, Spelling::cwd());
    }
    return run_format(request);
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(dispatch<Format>);

}  // namespace

}  // namespace clice::driver
