module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument;
import :command.database;
import :command.file_kind;
import :command.table;
import :command.toolchain;
import :config.config;
import :driver.driver;
import :project.configuration;
import :project.load;
import :project.open_index;
import :project.project;
import :support.shell;
import :vfs.cygwin;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;
import :worker.serialize;

namespace clice::driver {

namespace {

struct FileOptions {
    kota::deco::decl::HelpOption help;
    FileTarget target;

    DecoFlag(names = {"--json"},
             help = "Print the commands as JSON on stdout instead of text",
             required = false)
    json;

    WorkspaceOptions where;
    LogLevelOption log;
};

struct ConvertOptions {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<DATABASE>",
              help = "The compile_commands.json to convert, relative to the current directory",
              required = false)
    <std::vector<std::string>> database;

    DecoFlag(names = {"--json"},
             help = "Print the converted command as a database entry instead of text",
             required = false)
    json;

    WorkspaceOptions where;
    LogLevelOption log;
};

/// An entry of a compile_commands.json as written: beyond `directory` and
/// `file`, strings in every entry clice reads, its fields stay the JSON
/// values they are, to be written back unchanged.
struct WrittenEntry {
    std::string directory;
    std::string file;
    kota::ipc::protocol::optional<kota::codec::RawValue> arguments;
    kota::ipc::protocol::optional<kota::codec::RawValue> command;
    kota::ipc::protocol::optional<kota::codec::RawValue> output;

    /// The command as clice reads it: the `arguments` when they are an
    /// array, else the `command` split as the host's shell splits it, or
    /// as sh does in a database written inside Cygwin or MSYS2.
    std::vector<std::string> argv() const {
        std::vector<std::string> argv;
        if(arguments && kota::codec::json::from_string(arguments->data, argv)) {
            return argv;
        }
        std::string text;
        if(command) {
            kota::codec::json::from_string(command->data, text);
        }
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        auto tokens = is_posix_path(directory) ? tokenize_sh_command(text, saver)
                                               : tokenize_command(text, saver);
        return {tokens.begin(), tokens.end()};
    }
};

/// An entry of a compile_commands.json, its command as arguments.
struct DatabaseEntry {
    std::string directory;
    std::string file;
    std::vector<std::string> arguments;
    kota::ipc::protocol::optional<kota::codec::RawValue> output;
};

/// A command and where it runs.
struct CommandLine {
    std::string directory;
    std::vector<std::string> arguments;
};

/// One of a file's database entries and the command clice compiles the
/// file with under it.
struct EntryCommands {
    /// The compile_commands.json holding the entry.
    std::string database;

    /// The entry as written, a `command` split as clice splits it.
    DatabaseEntry entry;

    /// The entry's command with the rules applied and resolved through the
    /// compiler: what clice compiles with.
    CommandLine resolved;

    /// Why the compiler could not be queried, which leaves `resolved` the
    /// driver command clang's own driver reads in its place; absent on
    /// success.
    kota::ipc::protocol::optional<std::string> toolchain_error;
};

struct FileCommands {
    std::string file;
    std::vector<EntryCommands> commands;
};

/// The project of the workspace a command names, with its configuration,
/// and the build configuration that applies.
struct Workspace {
    Spelling root;
    std::unique_ptr<FileTable> files = std::make_unique<FileTable>();
    std::unique_ptr<Project> project = std::make_unique<Project>(*files);
    std::string configuration;
};

std::expected<Workspace, std::string> open_workspace(const WorkspaceOptions& where) {
    Workspace workspace{.root = where.root()};
    if(!vfs::is_directory(workspace.root)) {
        return std::unexpected(std::format("{}: not a directory", workspace.root));
    }
    workspace.files->spell_root(workspace.root);
    auto& config = workspace.project->config;
    config = Config::load_from_workspace(CanonicalPath(workspace.root));
    auto requested = where.configuration.tag();
    if(auto checked = check_requested_configuration(config, requested); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    workspace.configuration = resolve_configuration(config, requested);
    return workspace;
}

/// A compile_commands.json as one read found it: its entries, each as
/// written, and the hash of its bytes, which a load of the same bytes
/// records (CompilationDatabase::inputs). Only then do the load's entries
/// pair with these by position.
struct DatabaseText {
    std::vector<kota::codec::RawValue> entries;
    std::uint64_t hash = 0;

    bool loaded_by(const CompilationDatabase& cdb, SourceID source) const {
        return cdb.inputs(source).front().hash == hash;
    }
};

std::expected<DatabaseText, std::string> read_database(const Spelling& path) {
    auto observed = vfs::read_observed(path);
    if(!observed) {
        return std::unexpected(std::format("cannot read {}: {}", path, observed.error().message()));
    }
    DatabaseText text{.hash = observed->obs.hash};
    auto repaired = repaired_database_text(observed->content->getBuffer());
    if(auto result = kota::codec::json::from_string(repaired ? llvm::StringRef(*repaired)
                                                             : observed->content->getBuffer(),
                                                    text.entries);
       !result) {
        return std::unexpected(
            std::format("{} holds no array of entries: {}", path, result.error().message));
    }
    return text;
}

/// The command a ref compiles with.
struct Rendered {
    std::vector<std::string> arguments;

    /// Why the compiler could not be queried, which leaves `arguments` the
    /// driver command clang's own driver reads in its place.
    std::optional<std::string> error;
};

Rendered render_command(Project& project, const CommandRef& ref) {
    auto path = project.cdb.spelling(ref.file);
    auto resolution = project.toolchain.resolve(ref.command, ref.input);
    if(resolution && resolution->query_failure.empty()) {
        // The resolution is cached: rendering resolves it the same.
        auto argv = project.toolchain.render(ref, path).argv;
        return {
            .arguments = {argv.begin(), argv.end()}
        };
    }
    // Shown as clang's own driver reads it, a command whose compiler could
    // not be queried reads the same on every machine; its cc1 names the
    // machine's own headers.
    auto argv = project.commands.render_driver(ref, path);
    return {
        .arguments = {argv.begin(), argv.end()},
        .error = resolution ? std::move(resolution->query_failure) : std::move(resolution.error()),
    };
}

/// A loaded entry as its database writes it; nullopt when the database
/// changed since it loaded.
std::optional<DatabaseEntry> written_entry(const CompilationDatabase& cdb,
                                           const CompilationEntry& loaded) {
    auto text = read_database(Spelling::absolute(cdb.source_path(loaded.source)));
    if(!text || !text->loaded_by(cdb, loaded.source)) {
        return std::nullopt;
    }
    WrittenEntry entry;
    if(!kota::codec::json::from_string(text->entries[loaded.ordinal].data, entry)) {
        return std::nullopt;
    }
    return DatabaseEntry{.directory = std::move(entry.directory),
                         .file = std::move(entry.file),
                         .arguments = entry.argv(),
                         .output = std::move(entry.output)};
}

void print_text(const FileCommands& commands) {
    for(std::size_t i = 0; i < commands.commands.size(); i += 1) {
        auto& [database, entry, resolved, error] = commands.commands[i];
        if(i != 0) {
            driver::println();
        }
        driver::println("{}:", database);
        driver::println("  directory: {}", resolved.directory);
        driver::println("  command:   {}", join_command(entry.arguments));
        driver::println("  resolved:  {}", join_command(resolved.arguments));
        if(error) {
            driver::println("  error:     the compiler could not be queried: {}", *error);
        }
    }
}

int run_file(const FileOptions& opts) {
    auto args = positional(opts.target.inputs.as_optional(), {"<PATH>"});
    if(!args) {
        return usage_error(Errors::Text, args.error());
    }
    auto workspace = open_workspace(opts.where);
    if(!workspace) {
        LOG_ERROR("{}", workspace.error());
        return 1;
    }
    auto& project = *workspace->project;
    auto path = inspected_path(project, args->front());
    if(!vfs::is_file(path)) {
        LOG_ERROR("no such file: {}", path);
        return 1;
    }
    auto& files = project.file_table;
    auto file = files.intern(path);
    CanonicalRef identity = files.resolve(file);
    CanonicalPath root(workspace->root);
    load_sources(project,
                 root,
                 workspace->configuration,
                 compile_commands_above(identity.parent(), root));
    auto entries = project.build.entries(file);
    if(entries.empty()) {
        LOG_ERROR("No compilation database lists {}", files.display(file));
        return 1;
    }
    llvm::SmallVector<CommandRef> refs;
    for(auto& entry: entries) {
        refs.push_back(project.build.resolve(file,
                                             entry.command,
                                             CommandSource::CDBExact,
                                             identity,
                                             identity));
    }
    project.toolchain.warm(refs);
    FileCommands commands{.file = files.display(file)};
    assert(std::ranges::size(entries) == std::ranges::size(refs));
    for(auto [entry, ref]: std::views::zip(entries, refs)) {
        auto written = written_entry(project.cdb, entry);
        auto database = Spelling::absolute(project.cdb.source_path(entry.source));
        if(!written) {
            LOG_ERROR("{} changed while it was read; run the command again", database);
            return 1;
        }
        auto rendered = render_command(project, ref);
        auto directory = Spelling::absolute(project.commands.get(ref.command).directory);
        commands.commands.push_back({
            .database = files.display(CanonicalPath(database)),
            .entry = std::move(*written),
            .resolved = {.directory = files.display(CanonicalPath(directory)),
                         .arguments = std::move(rendered.arguments)},
            .toolchain_error = std::move(rendered.error),
        });
    }
    if(opts.json) {
        print_json(commands);
    } else {
        print_text(commands);
    }
    return 0;
}

/// The C-family sources `command` names among its arguments, as its
/// driver reads them: a file clang knows as C-family code by its suffix or
/// by the language selector before it, anchored where the compile runs,
/// `directory` moved by -working-directory.
llvm::SmallVector<Spelling> compiled_sources(llvm::ArrayRef<std::string> command,
                                             const Spelling& directory) {
    auto family = std::ranges::contains(command, "--driver-mode=cl")
                      ? CompilerFamily::ClangCL
                      : driver_family(command.front());
    std::vector<std::string> args(command.begin() + 1, command.end());
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true,
                                              .visibility = family_visibility(family)};
    auto anchor = directory;
    std::string language;
    llvm::SmallVector<std::string> inputs;
    for(auto& parsed: option::table().parse(args, options)) {
        if(!parsed) {
            continue;
        }
        auto id = parsed->id;
        if(id == option::OPT_working_directory) {
            anchor = Spelling(llvm::StringRef(parsed->values[0]), directory);
        } else if(id == option::OPT_x) {
            llvm::StringRef selected(parsed->values[0]);
            language = selected == "none" ? "" : selected.str();
        } else if(id == option::OPT__SLASH_Tc || id == option::OPT__SLASH_Tp) {
            inputs.push_back(llvm::StringRef(parsed->values[0]).str());
        } else if(id == option::OPT_INPUT) {
            auto input = parsed->values.empty() ? llvm::StringRef(parsed->spelling)
                                                : llvm::StringRef(parsed->values[0]);
            if(language.empty() ? is_c_family_file(input)
                                : is_c_family_type(clang::driver::types::lookupTypeForTypeSpecifier(
                                      language.c_str()))) {
                inputs.push_back(input.str());
            }
        }
    }
    llvm::SmallVector<Spelling> sources;
    for(auto& input: inputs) {
        sources.emplace_back(input, anchor);
    }
    return sources;
}

/// Convert the command given after --, which runs in the current
/// directory.
int convert_command(Workspace& workspace, llvm::ArrayRef<std::string> command, bool json) {
    auto directory = Spelling::cwd();
    auto sources = compiled_sources(command, directory);
    if(sources.size() != 1) {
        LOG_ERROR("The command names {} among its arguments; convert takes a command compiling one",
                  sources.empty()
                      ? "no C-family source"
                      : std::format("{} C-family sources ({})",
                                    sources.size(),
                                    llvm::join(std::views::transform(sources,
                                                                     [](const Spelling& source) {
                                                                         return source.str();
                                                                     }),
                                               ", ")));
        return 1;
    }
    auto& project = *workspace.project;
    auto& cdb = project.cdb;
    project.commands.set_workspace_root(CanonicalPath(workspace.root));
    project.build.reset_active(workspace.configuration);

    llvm::SmallVector<const char*> argv;
    for(auto& argument: command) {
        argv.push_back(argument.c_str());
    }
    auto entry = cdb.add_command(directory.str(), sources.front().str(), argv);
    if(!entry) {
        LOG_ERROR("Not a compile command: {}", join_command(command));
        return 1;
    }
    CanonicalRef identity = project.file_table.resolve(entry->file);
    auto ref = project.build.resolve(entry->file,
                                     entry->command,
                                     CommandSource::CDBExact,
                                     identity,
                                     identity);
    project.toolchain.warm(ref);
    auto rendered = render_command(project, ref);
    if(json) {
        print_json(DatabaseEntry{.directory = directory.str(),
                                 .file = sources.front().str(),
                                 .arguments = rendered.arguments});
    } else {
        driver::println("{}", join_command(rendered.arguments));
    }
    if(rendered.error) {
        LOG_ERROR("The command stays a driver command: {}", *rendered.error);
        return 1;
    }
    return 0;
}

/// Convert every entry of the database `path` names, a .json file or a
/// directory holding a compile_commands.json, printing the database of
/// converted entries.
int convert_database(Workspace& workspace, const Spelling& path) {
    if(path::extension(path.str()) != ".json" && !vfs::is_directory(path)) {
        LOG_ERROR(
            "{}: a compilation database is a .json file, or a directory holding a "
            "compile_commands.json",
            path);
        return 1;
    }
    auto& project = *workspace.project;
    auto& cdb = project.cdb;
    auto& files = project.file_table;
    project.commands.set_workspace_root(CanonicalPath(workspace.root));
    project.build.reset_active(workspace.configuration);
    auto source = cdb.add_source(path);
    auto database = Spelling::absolute(cdb.source_path(source));
    auto written = read_database(database);
    if(!written) {
        LOG_ERROR("{}", written.error());
        return 1;
    }
    if(!cdb.load_source(source)) {
        LOG_ERROR("{} cannot be loaded; the error above says why", database);
        return 1;
    }
    if(!written->loaded_by(cdb, source)) {
        LOG_ERROR("{} changed while it was read; run the command again", database);
        return 1;
    }

    llvm::DenseMap<std::uint32_t, CommandRef> refs;
    std::vector<CommandRef> all;
    for(auto& entry: cdb.entries()) {
        CanonicalRef identity = files.resolve(entry.file);
        auto ref = project.build.resolve(entry.file,
                                         entry.command,
                                         CommandSource::CDBExact,
                                         identity,
                                         identity);
        refs.try_emplace(entry.ordinal, ref);
        all.push_back(ref);
    }
    project.toolchain.warm(all);

    // A relative directory is relative to the database's own; written out,
    // the database is read from anywhere.
    auto base = database.parent();
    std::size_t unconverted = 0;
    std::vector<kota::codec::RawValue> converted;
    converted.reserve(written->entries.size());
    for(std::uint32_t index = 0; index < written->entries.size(); index += 1) {
        auto& value = written->entries[index];
        WrittenEntry entry;
        // A malformed entry, which clice skips, is left as it is.
        if(!kota::codec::json::from_string(value.data, entry)) {
            converted.push_back(std::move(value));
            continue;
        }
        auto ref = refs.find(index);
        if(!is_posix_path(entry.directory)) {
            entry.directory = Spelling(entry.directory, base).str();
        } else if(ref != refs.end()) {
            // A database written inside Cygwin or MSYS2 converts to the
            // Windows paths clice read it as.
            entry.directory = project.commands.get(ref->second.command).directory;
            entry.file = files.resolve(ref->second.file).str();
        }
        // The command of one clice does not compile, another language's
        // source, is left as it is too.
        if(ref != refs.end()) {
            auto rendered = render_command(project, ref->second);
            if(rendered.error) {
                unconverted += 1;
            }
            entry.arguments = to_raw(rendered.arguments);
            entry.command = std::nullopt;
        }
        converted.push_back(to_raw(entry));
    }
    print_json(converted);
    if(unconverted != 0) {
        LOG_ERROR("{} of the entries stay driver commands: the errors above say why", unconverted);
        return 1;
    }
    return 0;
}

int run_convert(const ConvertOptions& opts, llvm::ArrayRef<std::string> command) {
    auto args = positional(opts.database.as_optional(), {"<DATABASE>"}, 1);
    if(!args) {
        return usage_error(Errors::Text, args.error());
    }
    if(args->empty() == command.empty()) {
        return usage_error(Errors::Text,
                           "convert takes a compile_commands.json, or a command after --");
    }
    if(command.empty() && opts.json) {
        return usage_error(Errors::Text,
                           "--json converts a command; a database converts to a database");
    }
    auto workspace = open_workspace(opts.where);
    if(!workspace) {
        LOG_ERROR("{}", workspace.error());
        return 1;
    }
    if(!command.empty()) {
        return convert_command(*workspace, command, static_cast<bool>(opts.json));
    }
    return convert_database(*workspace, Spelling(args->front(), Spelling::cwd()));
}

/// `clice command convert`, whose options end at a `--`: what follows is
/// the command to convert.
CommandHandler convert_operation() {
    auto command = std::make_shared<std::vector<std::string>>();
    auto parse = operation<ConvertOptions>(
        "clice command convert [<DATABASE>] [OPTIONS] [-- <COMMAND>...]",
        "command",
        Errors::Text,
        [command](const ConvertOptions& opts) { return run_convert(opts, *command); });
    return [command, parse = std::move(parse)](std::span<std::string> args) mutable {
        auto dash = std::ranges::find(args, "--");
        command->assign(dash == args.end() ? dash : std::next(dash), args.end());
        return parse(args.first(static_cast<std::size_t>(dash - args.begin())));
    };
}

CommandHandler build(const char*) {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "convert",
        .description = "Convert a compilation database, or one command, to the commands clice "
                       "compiles with",
        .run = convert_operation(),
    });
    return with_operations(
        "",
        std::move(operations),
        Errors::Text,
        operation<FileOptions>("clice command <PATH> [OPTIONS]", "command", Errors::Text, run_file),
        /*main_targets=*/true);
}

[[maybe_unused]] const bool registered = register_command(
    Command::command,
    {.description = "Show the commands a file compiles with, or convert compile commands",
     .build = build});

}  // namespace

}  // namespace clice::driver
