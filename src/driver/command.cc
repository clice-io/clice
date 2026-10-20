module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument_parser;
import :command.command;
import :command.toolchain;
import :config.config;
import :driver.driver;
import :project.configuration;
import :project.load;
import :project.open_index;
import :project.project;
import :support.shell;
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

/// An entry of a compile_commands.json, as its fields are written.
struct DatabaseEntry {
    std::string directory;
    std::string file;
    kota::ipc::protocol::optional<std::vector<std::string>> arguments;
    kota::ipc::protocol::optional<std::string> command;
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
    /// driver command clice compiles with instead; absent on success.
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

/// The entries of the compile_commands.json at `path`, each as written.
std::expected<std::vector<kota::codec::RawValue>, std::string> read_database(const Spelling& path) {
    auto buffer = vfs::read(path);
    if(!buffer) {
        return std::unexpected(std::format("cannot read {}: {}", path, buffer.error().message()));
    }
    std::vector<kota::codec::RawValue> entries;
    if(auto result = kota::codec::json::from_string((*buffer)->getBuffer(), entries); !result) {
        return std::unexpected(
            std::format("{} holds no array of entries: {}", path, result.error().message));
    }
    return entries;
}

/// The command a ref compiles with.
struct Rendered {
    std::vector<std::string> arguments;

    /// Why the compiler could not be queried, which leaves `arguments` the
    /// driver command clice falls back to.
    std::optional<std::string> error;
};

Rendered render_command(CompilationDatabase& cdb, const CommandRef& ref) {
    auto resolved = cdb.toolchain().resolve(ref.config, ref.input);
    auto argv = resolved ? cdb.render(ref) : cdb.render_driver(ref);
    Rendered rendered{
        .arguments = {argv.begin(), argv.end()}
    };
    if(!resolved) {
        rendered.error = std::move(resolved.error());
    }
    return rendered;
}

/// The entry at position `index` of the database `path`, as written;
/// nullopt when the database changed since it loaded.
std::optional<DatabaseEntry> written_entry(const Spelling& path, std::uint32_t index) {
    auto entries = read_database(path);
    if(!entries || index >= entries->size()) {
        return std::nullopt;
    }
    DatabaseEntry entry;
    if(!kota::codec::json::from_string((*entries)[index].data, entry)) {
        return std::nullopt;
    }
    if(!entry.arguments) {
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        auto tokens = tokenize_command(entry.command.value_or(""), saver);
        entry.arguments.emplace(tokens.begin(), tokens.end());
        entry.command = std::nullopt;
    }
    return entry;
}

void print_text(const FileCommands& commands) {
    for(std::size_t i = 0; i < commands.commands.size(); i += 1) {
        auto& [database, entry, resolved, error] = commands.commands[i];
        if(i != 0) {
            driver::println();
        }
        driver::println("{}:", database);
        driver::println("  directory: {}", resolved.directory);
        driver::println("  command:   {}", join_command(*entry.arguments));
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
    CanonicalPath root(workspace->root);
    load_sources(project,
                 root,
                 workspace->configuration,
                 compile_commands_above(CanonicalPath(path.parent()), root));

    auto& files = project.file_table;
    auto file = files.intern(path);
    auto entries = project.build.entries(file);
    if(entries.empty()) {
        LOG_ERROR("No compilation database lists {}", files.display(file));
        return 1;
    }
    CanonicalRef identity = files.resolve(file);
    llvm::SmallVector<CommandRef> refs;
    for(auto& entry: entries) {
        refs.push_back(
            project.build.resolve(file, entry.config, CommandSource::CDBExact, identity, identity));
    }
    project.cdb.warm(refs);
    FileCommands commands{.file = files.display(file)};
    for(auto [entry, ref]: llvm::zip_equal(entries, refs)) {
        auto database = Spelling::absolute(project.cdb.source_path(entry.source));
        auto written = written_entry(database, entry.ordinal);
        if(!written) {
            LOG_ERROR("{} changed while it was read; run the command again", database);
            return 1;
        }
        auto rendered = render_command(project.cdb, ref);
        auto directory = Spelling::absolute(project.cdb.config(ref.config).directory);
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

/// The C-family sources `command` compiles, as its driver reads its
/// arguments.
llvm::SmallVector<std::string> compiled_sources(llvm::ArrayRef<std::string> command) {
    auto family = Toolchain::driver_family(command.front());
    std::vector<std::string> args(command.begin() + 1, command.end());
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true,
                                              .visibility = family_visibility(family)};
    llvm::SmallVector<std::string> sources;
    for(auto& parsed: option::table().parse(args, options)) {
        if(!parsed || (parsed->id != option::OPT_INPUT && parsed->id != option::OPT__SLASH_Tc &&
                       parsed->id != option::OPT__SLASH_Tp)) {
            continue;
        }
        auto input = parsed->values.empty() ? llvm::StringRef(parsed->spelling)
                                            : llvm::StringRef(parsed->values[0]);
        if(is_c_family_file(input)) {
            sources.push_back(input.str());
        }
    }
    return sources;
}

/// Convert the command given after --, which runs in the current
/// directory.
int convert_command(Workspace& workspace, llvm::ArrayRef<std::string> command, bool json) {
    auto sources = compiled_sources(command);
    if(sources.size() != 1) {
        LOG_ERROR("The command compiles {}; convert takes a command compiling one",
                  sources.empty() ? "no C-family source"
                                  : std::format("{} C-family sources ({})",
                                                sources.size(),
                                                llvm::join(sources, ", ")));
        return 1;
    }
    auto& project = *workspace.project;
    auto& cdb = project.cdb;
    CanonicalPath root(workspace.root);
    cdb.set_workspace_root(root);
    project.build.reset_active(workspace.configuration);

    auto directory = Spelling::cwd();
    llvm::SmallVector<const char*> argv;
    for(auto& argument: command) {
        argv.push_back(argument.c_str());
    }
    auto config = cdb.intern_command(directory, argv);
    if(!config) {
        LOG_ERROR("Not a compile command: {}", join_command(command));
        return 1;
    }
    auto file = project.file_table.intern(Spelling(sources.front(), directory));
    CanonicalRef identity = project.file_table.resolve(file);
    auto ref = project.build.resolve(file, *config, CommandSource::CDBExact, identity, identity);
    cdb.warm(ref);
    auto rendered = render_command(cdb, ref);
    if(json) {
        print_json(DatabaseEntry{.directory = directory.str(),
                                 .file = sources.front(),
                                 .arguments = rendered.arguments});
    } else {
        driver::println("{}", join_command(rendered.arguments));
    }
    if(rendered.error) {
        LOG_ERROR("The compiler could not be queried, so the command is clice's own reading of it");
        return 1;
    }
    return 0;
}

/// Convert every entry of the database at `path`, printing the database of
/// converted entries.
int convert_database(Workspace& workspace, const Spelling& path) {
    auto written = read_database(path);
    if(!written) {
        LOG_ERROR("{}", written.error());
        return 1;
    }
    auto& project = *workspace.project;
    auto& cdb = project.cdb;
    auto& files = project.file_table;
    cdb.set_workspace_root(CanonicalPath(workspace.root));
    project.build.reset_active(workspace.configuration);
    auto source = cdb.add_source(path);
    if(!cdb.load_source(source)) {
        LOG_ERROR("{} changed while it was read; run the command again", path);
        return 1;
    }

    llvm::DenseMap<std::uint32_t, CommandRef> refs;
    std::vector<CommandRef> all;
    for(auto& entry: cdb.entries()) {
        CanonicalRef identity = files.resolve(entry.file);
        auto ref = project.build.resolve(entry.file,
                                         entry.config,
                                         CommandSource::CDBExact,
                                         identity,
                                         identity);
        refs.try_emplace(entry.ordinal, ref);
        all.push_back(ref);
    }
    cdb.warm(all);

    // A relative directory is relative to the database's own; written out,
    // the database is read from anywhere.
    auto base = Spelling::absolute(path).parent();
    std::size_t unresolved = 0;
    std::vector<kota::codec::RawValue> converted;
    converted.reserve(written->size());
    for(std::uint32_t index = 0; index < written->size(); index += 1) {
        auto& value = (*written)[index];
        auto ref = refs.find(index);
        DatabaseEntry entry;
        // An entry clice skips (another language's source, a malformed
        // one) is left as it is.
        if(ref == refs.end() || !kota::codec::json::from_string(value.data, entry)) {
            converted.push_back(std::move(value));
            continue;
        }
        auto rendered = render_command(cdb, ref->second);
        if(rendered.error) {
            unresolved += 1;
        }
        entry.directory = Spelling(entry.directory, base).str();
        entry.arguments = std::move(rendered.arguments);
        entry.command = std::nullopt;
        converted.push_back(to_raw(entry));
    }
    print_json(converted);
    if(unresolved != 0) {
        LOG_ERROR(
            "{} of the converted entries could not be resolved: the compiler could not be "
            "queried, so they are clice's own reading of their command",
            unresolved);
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
