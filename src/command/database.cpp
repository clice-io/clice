module clice;

import :command.argument;
import :command.database;
import :command.file_kind;
import :command.normalize;
import :command.table;
import :support.logging;
import :support.process;
import :support.text;
import :vfs.cygwin;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// How far past a document's end simdjson reads (SIMDJSON_PADDING, a
/// constant the module does not carry): with that much room past the text,
/// the parse reads the database where it was read into, else in a copy.
constexpr std::size_t json_padding = 64;

/// The registered path of a source: the database file (a path without the
/// .json extension names a directory, existing or not, holding
/// compile_commands.json) in the identity of the directory holding it —
/// every spelling of that directory finds the same source, while a
/// database file symlinked elsewhere is still read through the link. So
/// is a directory that is itself a symlink (build -> out/debug): switching
/// the link switches the database.
Spelling source_key(const Spelling& path) {
    auto file = path::extension(path) != ".json" ? Spelling("compile_commands.json", path) : path;
    auto directory = file.parent();
    if(fs::is_symlink(directory)) {
        return Spelling(
            path::filename(file),
            Spelling(path::filename(directory), Spelling(CanonicalPath(directory.parent()))));
    }
    return Spelling(path::filename(file), Spelling(CanonicalPath(directory)));
}

/// The per-file delta between two command hash snapshots.
CDBDiff diff_snapshots(const llvm::DenseMap<Fid, llvm::SmallVector<std::string, 1>>& before,
                       const llvm::DenseMap<Fid, llvm::SmallVector<std::string, 1>>& after) {
    CDBDiff diff;

    for(auto& bucket: after) {
        auto it = before.find(bucket.first);
        if(it == before.end()) {
            diff.added.push_back(bucket.first);
        } else if(it->second != bucket.second) {
            diff.changed.push_back(bucket.first);
        }
    }

    for(auto& bucket: before) {
        if(!after.contains(bucket.first)) {
            diff.removed.push_back(bucket.first);
        }
    }

    ranges::sort(diff.added);
    ranges::sort(diff.removed);
    ranges::sort(diff.changed);

    return diff;
}

/// What simdjson says is wrong, without the error code in front and the
/// note that the error is fatal after.
std::string json_problem(simdjson::error_code error) {
    llvm::StringRef message = simdjson::error_message(error);
    if(auto [code, rest] = message.split(": ");
       !rest.empty() && ranges::all_of(code, [](char c) { return llvm::isUpper(c) || c == '_'; })) {
        message = rest;
    }
    return message.take_front(message.find("This is a fatal")).trim().rtrim('.').str();
}

/// The text of the string `value` of the database `json`: a view of the
/// database's own bytes when it holds no escape, else unescaped into
/// `storage`. simdjson's own get_string writes every string of the document
/// into one buffer, as large as the database by its end. `error` says why
/// there is none.
std::optional<std::string_view> string_of(auto&& value,
                                          llvm::StringRef json,
                                          simdjson::ondemand::parser& parser,
                                          std::string& storage,
                                          simdjson::error_code& error) {
    simdjson::ondemand::raw_json_string raw;
    if((error = value.get_raw_json_string().get(raw))) {
        return std::nullopt;
    }
    const char* first = raw.raw();
    const char* end = json.data() + json.size();
    auto* quote = static_cast<const char*>(std::memchr(first, '"', end - first));
    if(!std::memchr(first, '\\', quote - first)) {
        return std::string_view(first, quote - first);
    }
    const char* last = first;
    for(; *last != '"'; last += 1) {
        if(*last == '\\') {
            last += 1;
        }
    }
    storage.resize(last - first + json_padding);
    auto* out = reinterpret_cast<std::uint8_t*>(storage.data());
    std::string_view text;
    if((error = parser.unescape(raw, out).get(text))) {
        return std::nullopt;
    }
    return text;
}

/// The response files a command line or response file text names: its
/// `@`-tokens, quotes taken off. Close enough to read ahead by.
void named_responses(llvm::StringRef text, std::vector<std::string>& names) {
    auto starts_token = [&](std::size_t at) {
        return at == 0 || llvm::isSpace(text[at - 1]);
    };
    for(auto at = text.find('@'); at != llvm::StringRef::npos; at = text.find('@', at + 1)) {
        if(!starts_token(at) && !(text[at - 1] == '"' && starts_token(at - 1))) {
            continue;
        }
        auto rest = text.drop_front(at + 1);
        auto name = rest.consume_front("\"") ? rest.take_until([](char c) { return c == '"'; })
                                             : rest.take_until(llvm::isSpace);
        names.emplace_back(name.trim('"'));
    }
}

/// A path the load will intern, read ahead of it.
struct ReadAhead {
    std::string path;

    /// Its identity, resolved ahead.
    std::optional<CanonicalPath> identity;

    /// A response file's content.
    std::optional<ObservedFile> content;
};

/// Read the response files the entries of the database `json` name, and
/// those they name in turn, and resolve their identities and those of the
/// files the entries compile, on the thread pool before the load reads and
/// interns them one by one: a cold disk serves reads made together far
/// faster, and an identity is a system call each, which on Windows costs as
/// much as the rest of the load (Unreal names two response files from each
/// of its 40000 commands). The response files' contents are kept for the
/// load, whose one thread would read each again (Normalizer::read_responses).
std::deque<ReadAhead> read_ahead(llvm::StringRef json,
                                 simdjson::padded_string_view padded,
                                 const Spelling& database_dir) {
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    simdjson::ondemand::array entries;
    if(parser.iterate(padded).get(doc) || doc.get_array().get(entries)) {
        return {};
    }
    // A deque: the rounds add to it while earlier tasks fill its files.
    std::deque<ReadAhead> resolved;
    // The response files of the round at hand, and the directory the
    // command naming each runs in, which names nested ones are relative to
    // as well.
    std::vector<std::pair<std::size_t, Spelling>> responses;
    llvm::StringSet<> seen;
    std::vector<std::string> names;
    std::string directory_storage, file_storage, command_storage, argument_storage;
    simdjson::error_code error;
    auto add = [&](std::string path) -> std::optional<std::size_t> {
        if(!seen.insert(path).second) {
            return std::nullopt;
        }
        resolved.push_back({.path = std::move(path)});
        return resolved.size() - 1;
    };
    for(auto element: entries) {
        simdjson::ondemand::object entry;
        if(element.get_object().get(entry)) {
            return {};
        }
        auto directory = string_of(entry["directory"], json, parser, directory_storage, error);
        auto file = string_of(entry["file"], json, parser, file_storage, error);
        // One written inside Cygwin or MSYS2 spells its paths for their mount
        // table, which only the load reads.
        if(!directory || !file || fs::cygwin::is_posix_path(*directory)) {
            return {};
        }
        auto base = Spelling(*directory, database_dir);
        add(Spelling(*file, base).str());
        names.clear();
        if(auto command = string_of(entry["command"], json, parser, command_storage, error)) {
            named_responses(*command, names);
        } else if(simdjson::ondemand::array arguments;
                  !entry["arguments"].get_array().get(arguments)) {
            for(auto argument: arguments) {
                auto text = string_of(argument, json, parser, argument_storage, error);
                if(text && text->starts_with('@')) {
                    names.emplace_back(text->substr(1));
                }
            }
        }
        for(auto& name: names) {
            if(auto index = add(Spelling(name, base).str())) {
                responses.emplace_back(*index, base);
            }
        }
    }
    auto read_all = [&]() -> async::task<> {
        llvm::DenseSet<std::size_t> read;
        for(auto& [index, directory]: responses) {
            read.insert(index);
        }
        std::vector<ReadAhead*> unread;
        for(std::size_t index = 0; index < resolved.size(); index += 1) {
            if(!read.contains(index)) {
                unread.push_back(&resolved[index]);
            }
        }
        // The identities of the files read in no round, beside the rounds.
        auto identify = [&](std::size_t, std::size_t first, std::size_t end) {
            for(auto* file: llvm::ArrayRef(unread).slice(first, end - first)) {
                file->identity.emplace(Spelling::absolute(file->path));
            }
        };
        async::task_group<> identifying;
        identifying.spawn(in_parts(unread.size(), identify));
        while(!responses.empty()) {
            std::vector<std::vector<std::string>> nested(responses.size());
            co_await in_parts(responses.size(),
                              [&](std::size_t, std::size_t first, std::size_t end) {
                                  for(auto index = first; index < end; index += 1) {
                                      auto& file = resolved[responses[index].first];
                                      file.identity.emplace(Spelling::absolute(file.path));
                                      if(auto read = vfs::read_observed(file.path)) {
                                          named_responses(read->text, nested[index]);
                                          file.content = std::move(*read);
                                      }
                                  }
                              });
            std::vector<std::pair<std::size_t, Spelling>> next;
            for(auto [response, named]: views::zip(responses, nested)) {
                for(auto& name: named) {
                    if(auto index = add(Spelling(name, response.second).str())) {
                        next.emplace_back(*index, response.second);
                    }
                }
            }
            responses = std::move(next);
        }
        co_await identifying.join();
    };
    async::run(read_all());
    return resolved;
}

}  // namespace

std::string per_file_option(const CommandTable& commands,
                            llvm::ArrayRef<CompilationEntry> entries) {
    llvm::DenseSet<std::uint32_t> distinct;
    for(auto& entry: entries) {
        distinct.insert(static_cast<std::uint32_t>(entry.command));
    }
    // A handful of entries tells nothing, and commands most entries share
    // are no problem.
    if(entries.size() < 64 || distinct.size() * 10 < entries.size() * 9) {
        return {};
    }
    llvm::StringMap<llvm::DenseSet<std::uint64_t>> values;
    for(auto id: distinct) {
        for(auto& arg: commands.get(CommandID(id)).args) {
            if(arg.cls == ArgClass::UserContent || arg.cls == ArgClass::Input) {
                continue;
            }
            if(arg.opt_id == clang_options::OPT_UNKNOWN) {
                auto [name, value] = llvm::StringRef(arg.spelling).split('=');
                values[name].insert(llvm::xxh3_64bits(value));
                continue;
            }
            std::string joined;
            for(llvm::StringRef value: arg.values) {
                joined += value;
                joined += '\0';
            }
            auto name = clang_options::table().option(arg.opt_id)->prefixed_name();
            values[name].insert(llvm::xxh3_64bits(joined));
        }
    }
    for(auto& entry: values) {
        if(entry.getValue().size() * 10 >= distinct.size() * 9) {
            return entry.getKey().str();
        }
    }
    return {};
}

std::optional<std::string> repaired_database_text(llvm::StringRef bytes) {
    if(simdjson::validate_utf8(bytes.data(), bytes.size())) {
        return std::nullopt;
    }
    return kotatsu::replace_invalid_utf8(bytes);
}

CompilationDatabase::CompilationDatabase(FileTable& files, CommandTable& commands) :
    file_table(files), command_table(commands) {}

void CompilationDatabase::sort_entries() {
    ranges::sort(entry_list, [](const CompilationEntry& a, const CompilationEntry& b) {
        return std::tie(a.file, a.source, a.ordinal) < std::tie(b.file, b.source, b.ordinal);
    });

    writers.clear();
    for(auto& entry: entry_list) {
        if(auto output = command_table.pch_output(entry.command)) {
            writers[file_table.intern(*output)].push_back(entry);
        }
    }
}

SourceID CompilationDatabase::add_source(const Spelling& path) {
    auto key = source_key(path);
    if(auto existing = find_source(key)) {
        return *existing;
    }
    auto file = file_table.intern(key);
    source_files.push_back({.path = std::move(key).str(), .inputs = {{.file = file}}});
    found_sources.clear();
    return SourceID(source_files.size() - 1);
}

std::optional<SourceID> CompilationDatabase::find_source(const Spelling& path) const {
    auto [it, inserted] = found_sources.try_emplace(path.str());
    if(inserted) {
        auto key = source_key(path);
        for(std::size_t i = 0; i < source_files.size(); i += 1) {
            if(source_files[i].path == key.str()) {
                it->second = SourceID(i);
                break;
            }
        }
    }
    return it->second;
}

std::optional<SourceID> CompilationDatabase::find_same_file(const Spelling& path) const {
    auto key = source_key(path);
    CanonicalPath file(key);
    for(std::size_t i = 0; i < source_files.size(); i += 1) {
        // The tests' anonymous source has no file.
        auto& registered = source_files[i].path;
        if(!registered.empty() && registered != key.str() &&
           CanonicalPath(Spelling::absolute(registered)) == file) {
            return SourceID(i);
        }
    }
    return std::nullopt;
}

llvm::StringRef CompilationDatabase::source_path(SourceID id) const {
    return source_files[static_cast<std::size_t>(id)].path;
}

CDBDiff CompilationDatabase::unload_source(SourceID id) {
    auto before = command_hash_snapshot();
    source_files[static_cast<std::size_t>(id)].loaded = false;
    std::erase_if(entry_list, [&](const CompilationEntry& entry) { return entry.source == id; });
    sort_entries();
    return diff_snapshots(before, command_hash_snapshot());
}

bool CompilationDatabase::loaded(SourceID id) const {
    return source_files[static_cast<std::size_t>(id)].loaded;
}

std::optional<std::size_t> CompilationDatabase::load(llvm::StringRef path) {
    return load_source(add_source(Spelling::absolute(path)));
}

std::optional<std::size_t> CompilationDatabase::load_source(SourceID id) {
    auto& source = source_files[static_cast<std::size_t>(id)];
    llvm::StringRef path = source.path;

    auto observed = vfs::read_observed(source.path, json_padding);
    if(!observed) {
        // A database not built yet is no failure: the callers wait for it.
        if(observed.error().code != std::errc::no_such_file_or_directory) {
            LOG_WARN("Cannot read the compilation database {}: {}",
                     path,
                     observed.error().code.message());
        }
        return std::nullopt;
    }
    auto database = file_table.intern(CanonicalPath(Spelling::absolute(source.path)));
    file_table.disk.observe(database, observed->obs);
    source.inputs = {
        {.file = database, .hash = observed->obs.hash}
    };
    // An entry holding a byte the repair replaced is skipped.
    auto repaired = repaired_database_text(observed->text);
    auto damaged = [&](llvm::StringRef value) {
        return repaired && value.contains("\xEF\xBF\xBD");
    };
    auto& json_buf = repaired ? *repaired : observed->text;
    // The padding is the text's own spaces: simdjson reads past a string's
    // end in wide blocks, and a string's spare capacity is no memory to read.
    auto padded = simdjson::pad(json_buf);
    Timer ahead;
    auto resolved = read_ahead(json_buf, padded, Spelling::absolute(path).parent());
    llvm::DenseMap<Fid, ObservedFile> read_responses;
    for(auto& file: resolved) {
        auto fid = file_table.intern(Spelling::absolute(file.path), *file.identity);
        if(file.content) {
            read_responses.try_emplace(fid, std::move(*file.content));
        }
    }
    LOG_PERF("startup",
             "phase=cdb_read_ahead files={} elapsed_ms={:.0f}",
             resolved.size(),
             ahead.precise_ms());
    simdjson::ondemand::parser json_parser;
    simdjson::ondemand::document doc;
    if(auto error = json_parser.iterate(padded).get(doc)) {
        LOG_GUIDANCE("The compilation database {} is not valid JSON ({}); it is not loaded",
                     path,
                     json_problem(error));
        return std::nullopt;
    }
    // The document reads as the entries are taken: a broken structure shows
    // as the entry it falls in failing to read, at the place it is.
    auto invalid = [&](simdjson::error_code error) {
        const char* at = nullptr;
        if(doc.current_location().get(at)) {
            LOG_GUIDANCE("The compilation database {} is not valid JSON ({}); it is not loaded",
                         path,
                         json_problem(error));
            return;
        }
        auto before = llvm::StringRef(json_buf).take_front(at - json_buf.data());
        auto line_start = before.rfind('\n');
        auto column =
            line_start == llvm::StringRef::npos ? before.size() + 1 : before.size() - line_start;
        LOG_GUIDANCE(
            "The compilation database {} is not valid JSON at line {}, column {} ({}); it is not "
            "loaded",
            path,
            before.count('\n') + 1,
            column,
            json_problem(error));
    };

    simdjson::ondemand::array arr;
    if(auto error = doc.get_array().get(arr)) {
        if(simdjson::is_fatal(error)) {
            invalid(error);
            return std::nullopt;
        }
        // PowerShell's ConvertTo-Json writes a one-entry array as the entry.
        simdjson::dom::parser checker;
        simdjson::dom::object single;
        if(!checker.parse(json_buf).get(single) && single["file"].is_string()) {
            LOG_GUIDANCE(
                "The compilation database {} is one entry, not an array of them (PowerShell's "
                "ConvertTo-Json writes a single entry so); wrap it in [ ]",
                path);
            return std::nullopt;
        }
        LOG_GUIDANCE("The compilation database {} is not an array of entries", path);
        return std::nullopt;
    }

    // Parse into a local vector and only swap it in at the end: a file that
    // fails to read or parse leaves the loaded entries intact, so
    // reload_and_diff() reports no change instead of dropping every file.
    // The CDB poll's two-tick debounce keeps half-written files from being
    // read at all.
    std::vector<CompilationEntry> new_entries;
    std::vector<std::string> unreadable;
    Normalizer normalizer{.commands = command_table,
                          .files = file_table,
                          .inputs = &source.inputs,
                          .unreadable = &unreadable,
                          .read_responses = std::move(read_responses)};

    // A file several entries name, through links, is spelled as the first
    // names it, whose command is its default.
    // LIMITATION: across databases the last loaded spells it, whichever
    // ranks first: two build directories whose databases name one source
    // through different links compile it under the first's command from
    // the other's path. A reload naming a source through another link, its
    // command the same, changes no command: an open document compiles
    // under the new name only at its next edit.
    llvm::DenseSet<Fid> named;

    // A database written inside Cygwin or MSYS2 (CMake run from their shell)
    // spells its paths POSIX-style: the mount table of the runtime on PATH,
    // found at the first such entry, maps the entries so spelled.
    std::optional<fs::cygwin::MountTable> table;

    // An entry's strings (string_of), each value's storage its own.
    std::string dir_storage, file_storage, command_storage, arg_storage;
    // Why the last read of an entry's value failed.
    simdjson::error_code read_error{};
    auto text_of = [&](auto&& value, std::string& storage) {
        return string_of(value, json_buf, json_parser, storage, read_error);
    };

    // The tokens of the entry at hand, their storage kept for the next.
    std::vector<std::string> args;
    llvm::SmallVector<std::string_view> views;
    llvm::StringMap<Spelling> working_dirs;
    llvm::DenseMap<SharedKey, CommandID> shared;
    // How many commands were read in full, and of them how many had no key;
    // the time keying them and reading them took.
    std::size_t read_commands = 0;
    std::size_t keyless = 0;
    double keying_ms = 0;
    double reading_ms = 0;
    std::uint32_t index = 0;
    std::uint32_t skipped = 0;
    bool broken = false;
    auto skip_entry = [&](llvm::StringRef reason) {
        if(simdjson::is_fatal(read_error)) {
            invalid(read_error);
            broken = true;
            return;
        }
        LOG_WARN("Skipping entry {} of the compilation database {}: {}", index, path, reason);
        skipped += 1;
    };
    for(auto element: arr) {
        if(broken) {
            break;
        }
        auto skip = llvm::make_scope_exit([&] { index += 1; });

        simdjson::ondemand::object obj;
        if((read_error = element.get_object().get(obj))) {
            skip_entry("it is not an object");
            continue;
        }

        std::string_view dir_sv, file_sv;
        if(auto text = text_of(obj["directory"], dir_storage)) {
            dir_sv = *text;
        } else {
            skip_entry("it has no \"directory\"");
            continue;
        }

        if(auto text = text_of(obj["file"], file_storage)) {
            file_sv = *text;
        } else {
            skip_entry("it has no \"file\"");
            continue;
        }

        std::optional<llvm::StringRef> command;
        simdjson::ondemand::array args_arr;
        if(!obj["arguments"].get_array().get(args_arr)) {
            bool malformed = false;
            std::size_t count = 0;
            for(auto arg_val: args_arr) {
                auto text = text_of(arg_val, arg_storage);
                if(!text) {
                    malformed = true;
                    break;
                }
                if(count < args.size()) {
                    args[count].assign(*text);
                } else {
                    args.emplace_back(*text);
                }
                count += 1;
            }
            args.resize(count);
            if(malformed && simdjson::is_fatal(read_error)) {
                invalid(read_error);
                return std::nullopt;
            }
            if(malformed || args.empty()) {
                continue;
            }
        } else {
            std::string_view cmd_sv;
            if(auto text = text_of(obj["command"], command_storage)) {
                cmd_sv = *text;
            } else {
                skip_entry("it has neither \"arguments\" nor \"command\"");
                continue;
            }
            command = llvm::StringRef(cmd_sv.data(), cmd_sv.size());
        }

        llvm::StringRef file_ref(file_sv.data(), file_sv.size());
        if(damaged(dir_sv) || damaged(file_ref) ||
           (command ? damaged(*command) : ranges::any_of(args, damaged))) {
            skip_entry(std::format("{} holds bytes that are not UTF-8", file_ref));
            continue;
        }

        llvm::StringRef dir_ref(dir_sv.data(), dir_sv.size());
        normalizer.mounts = nullptr;
        if(fs::cygwin::is_posix_path(dir_ref)) {
            if(!table) {
                // Another runtime's table maps the directory under its own
                // root; another of one kind, Git for Windows' MSYS2 beside
                // MSYS2, maps a drive's paths alike but lacks the compiler.
                std::vector<std::string> written = {dir_ref.str()};
                auto tokens = command ? split_command(*command, sys::cmd::Quoting::POSIX) : args;
                auto compiler = wrapper_prefix_len(tokens);
                if(compiler < tokens.size() && fs::cygwin::is_posix_path(tokens[compiler])) {
                    written.emplace_back(tokens[compiler]);
                }
                auto [found] = async::run(find_mount_table({}, std::move(written)));
                if(!*found) {
                    LOG_GUIDANCE(
                        "Compilation database {} spells its paths POSIX-style ({}), and they "
                        "cannot be mapped: {}. Put the bin directory of the Cygwin or MSYS2 "
                        "that wrote it on clice's PATH (in VS Code, clice.serverEnv); one "
                        "written outside them names its directories with a drive.",
                        path,
                        dir_ref,
                        found->error());
                    return std::nullopt;
                }
                table = std::move(**found);
            }
            normalizer.mounts = &*table;
        }

        // A relative `directory` anchors to the CDB file's own location —
        // self-contained, so every consumer of the same file (server,
        // batch, inspect) resolves it identically.
        auto directory = spelled(dir_ref, Spelling::absolute(path).parent(), normalizer.mounts);
        // The compile runs in the directory itself, whichever way the
        // database spells it: its identity keys the command.
        auto [known, fresh] = working_dirs.try_emplace(directory);
        if(fresh) {
            known->second = Spelling(CanonicalPath(directory));
        }
        auto& working = known->second;

        // Some build systems emit non-C-family files (e.g. .rc, .asm, .S, .def)
        // into compile_commands.json: skipped, unless the command's -x
        // says what clang does not know the extension for (an Arduino
        // `.ino` sketch); one clang knows as another language stays
        // skipped, as the server and inspect skip it.
        bool c_family = is_c_family_file(file_ref);

        auto file = spelled(file_ref, directory, normalizer.mounts);
        auto path_id = file_table.intern(file);
        if(named.insert(path_id).second) {
            file_table.intern_entry(file);
        }

        kotatsu::string_list_ref tokens =
            command ? normalizer.split(*command, views, args) : kotatsu::string_list_ref(args);
        Timer keying;
        auto key = normalizer.shared_key(working, file_ref, path_id, tokens);
        keying_ms += keying.precise_ms();
        std::optional<CommandID> normalized;
        if(auto known = key ? shared.find(*key) : shared.end(); known != shared.end()) {
            normalized = known->second;
        } else {
            Timer reading;
            normalized = normalizer.normalize(working, path_id, tokens);
            reading_ms += reading.precise_ms();
            read_commands += 1;
            keyless += key ? 0 : 1;
            // The command of a precompiled header's writer keeps its output.
            if(key && normalized && !command_table.pch_output(*normalized)) {
                shared.try_emplace(*key, *normalized);
            }
        }

        if(!normalized || is_other_language_path(file_ref) ||
           (!c_family && !command_table.forces_c_family(*normalized))) {
            continue;
        }
        new_entries.push_back(
            {.file = path_id, .command = *normalized, .source = id, .ordinal = index});
    }

    if(broken) {
        return std::nullopt;
    }
    LOG_PERF("startup",
             "phase=cdb_commands read={} keyless={} keying_ms={:.0f} reading_ms={:.0f}",
             read_commands,
             keyless,
             keying_ms,
             reading_ms);
    if(skipped != 0) {
        LOG_GUIDANCE("{} of the compilation database {} {} skipped; the log names {}",
                     counted(skipped, "entry", "entries"),
                     path,
                     skipped == 1 ? "is" : "are",
                     skipped == 1 ? "it" : "them");
    }

    if(auto option = per_file_option(command_table, new_entries); !option.empty()) {
        LOG_WARN(
            "Compilation database {}: nearly every entry has a command of its own, set apart by "
            "the value of {}, so clice resolves and scans each on its own. If {} names a file "
            "the build writes, clice should leave it out of the command: report it.",
            path,
            option,
            option);
    }
    auto count = new_entries.size();
    std::erase_if(entry_list, [&](const CompilationEntry& entry) { return entry.source == id; });
    entry_list.insert(entry_list.end(), new_entries.begin(), new_entries.end());
    source.loaded = true;
    source.present = true;
    auto responses = ranges::subrange(source.inputs.begin() + 1, source.inputs.end());
    ranges::sort(responses, {}, &LoadInput::file);
    auto duplicates = ranges::unique(responses, {}, &LoadInput::file);
    source.inputs.erase(duplicates.begin(), duplicates.end());
    if(!unreadable.empty()) {
        ranges::sort(unreadable);
        unreadable.erase(ranges::unique(unreadable).begin(), unreadable.end());
        LOG_GUIDANCE(
            "The compilation database {} names response files that cannot be read, and the "
            "commands naming them compile without their options: {}. A build may delete them "
            "once it has run; build again to bring them back",
            path,
            llvm::join(unreadable, ", "));
    }
    sort_entries();
    return count;
}

llvm::ArrayRef<LoadInput> CompilationDatabase::inputs(SourceID id) const {
    return source_files[static_cast<std::size_t>(id)].inputs;
}

bool CompilationDatabase::present(SourceID id) const {
    return source_files[static_cast<std::size_t>(id)].present;
}

void CompilationDatabase::set_present(SourceID id, bool present) {
    source_files[static_cast<std::size_t>(id)].present = present;
}

llvm::DenseMap<Fid, llvm::SmallVector<std::string, 1>>
    CompilationDatabase::command_hash_snapshot() {
    llvm::DenseMap<Fid, llvm::SmallVector<std::string, 1>> snapshot;
    for(auto& entry: entry_list) {
        snapshot[entry.file].push_back(command_table.command_hash_hex(entry.command));
    }
    return snapshot;
}

std::optional<CDBDiff> CompilationDatabase::reload_and_diff(SourceID id) {
    auto before = command_hash_snapshot();
    if(!load_source(id)) {
        // Unreadable or unparsable (e.g. still locked by the generator):
        // the old entries were kept, and the caller must not treat this as
        // "no change" — it has to retry.
        return std::nullopt;
    }
    return diff_snapshots(before, command_hash_snapshot());
}

llvm::ArrayRef<CompilationEntry> CompilationDatabase::candidate_entries(Fid path_id) const {
    auto [first, last] = ranges::equal_range(entry_list, path_id, {}, &CompilationEntry::file);
    if(first == last) {
        return {};
    }
    return {&*first, static_cast<std::size_t>(last - first)};
}

llvm::ArrayRef<CompilationEntry> CompilationDatabase::candidate_entries(llvm::StringRef file) {
    return candidate_entries(file_table.intern(Spelling::absolute(file)));
}

bool CompilationDatabase::has_entry(llvm::StringRef file) {
    return !candidate_entries(file).empty();
}

llvm::ArrayRef<CompilationEntry> CompilationDatabase::pch_writers(Fid pch) const {
    auto it = writers.find(pch);
    return it != writers.end() ? llvm::ArrayRef<CompilationEntry>(it->second)
                               : llvm::ArrayRef<CompilationEntry>();
}

std::optional<CompilationEntry>
    CompilationDatabase::append_test_command(Fid file, std::optional<CommandID> normalized) {
    if(!normalized) {
        return std::nullopt;
    }
    /// Commands added one at a time accumulate into one anonymous source;
    /// its path is empty, which no real database can have.
    auto anonymous =
        ranges::find_if(source_files, [](const Source& source) { return source.path.empty(); });
    if(anonymous == source_files.end()) {
        source_files.push_back({});
        anonymous = source_files.end() - 1;
    }
    auto id = SourceID(anonymous - source_files.begin());
    CompilationEntry entry{
        .file = file,
        .command = *normalized,
        .source = id,
        .ordinal =
            static_cast<std::uint32_t>(ranges::count(entry_list, id, &CompilationEntry::source)),
    };
    entry_list.push_back(entry);
    sort_entries();
    return entry;
}

std::optional<CompilationEntry>
    CompilationDatabase::add_command(llvm::StringRef directory,
                                     llvm::StringRef file,
                                     kotatsu::string_list_ref arguments) {
    auto base = Spelling::absolute(directory);
    auto fid = file_table.intern(Spelling(file, base));
    Normalizer normalizer{.commands = command_table, .files = file_table};
    return append_test_command(fid, normalizer.normalize(base, fid, arguments));
}

std::optional<CompilationEntry> CompilationDatabase::add_command(llvm::StringRef directory,
                                                                 llvm::StringRef file,
                                                                 llvm::StringRef command) {
    auto base = Spelling::absolute(directory);
    auto fid = file_table.intern(Spelling(file, base));
    Normalizer normalizer{.commands = command_table, .files = file_table};
    return append_test_command(fid, normalizer.normalize(base, fid, command));
}

}  // namespace clice
