module;

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"
#include "support/prelude.h"

module clice;

import :command.database;
import :command.file_kind;
import :command.normalize;
import :command.table;
import :support.anomaly;
import :support.logging;
import :support.shell;
import :vfs.cygwin;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// The registered path of a source: the database file (a path without the
/// .json extension names a directory, existing or not, holding
/// compile_commands.json) in the identity of the directory holding it —
/// every spelling of that directory finds the same source, while a
/// database file symlinked elsewhere is still read through the link. So
/// is a directory that is itself a symlink (build -> out/debug): switching
/// the link switches the database.
Spelling source_key(const Spelling& path) {
    auto file =
        path::extension(path.str()) != ".json" ? Spelling("compile_commands.json", path) : path;
    auto directory = file.parent();
    if(vfs::is_symlink(directory.str())) {
        return Spelling(
            path::filename(file.str()),
            Spelling(path::filename(directory.str()), Spelling(CanonicalPath(directory.parent()))));
    }
    return Spelling(path::filename(file.str()), Spelling(CanonicalPath(directory)));
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

}  // namespace

std::optional<std::string> repaired_database_text(llvm::StringRef bytes) {
    if(llvm::json::isUTF8(bytes)) {
        return std::nullopt;
    }
    return llvm::json::fixUTF8(bytes);
}

CompilationDatabase::CompilationDatabase(FileTable& files, CommandTable& commands) :
    file_table(files), command_table(commands) {}

void CompilationDatabase::rebuild_entry_list() {
    entry_list.clear();
    for(auto& source: source_files) {
        entry_list.insert(entry_list.end(), source.entries.begin(), source.entries.end());
    }
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
    source_files.push_back({.path = key.str(), .inputs = {{.file = file}}});
    return SourceID(source_files.size() - 1);
}

std::optional<SourceID> CompilationDatabase::find_source(const Spelling& path) const {
    auto key = source_key(path);
    for(std::size_t i = 0; i < source_files.size(); i += 1) {
        if(source_files[i].path == key.str()) {
            return SourceID(i);
        }
    }
    return std::nullopt;
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
    auto& source = source_files[static_cast<std::size_t>(id)];
    source.entries.clear();
    source.loaded = false;
    rebuild_entry_list();
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

    auto observed = vfs::read_observed(source.path);
    if(!observed) {
        // A database not built yet is no failure: the callers wait for it.
        if(observed.error() != std::errc::no_such_file_or_directory) {
            LOG_ERROR("Failed to read compilation database from {}: {}",
                      path,
                      observed.error().message());
        }
        return std::nullopt;
    }
    auto database = file_table.intern(CanonicalPath(Spelling::absolute(source.path)));
    file_table.disk.observe(database, observed->obs);
    source.inputs = {
        {.file = database, .hash = observed->obs.hash}
    };
    // An entry holding a byte the repair replaced is skipped.
    auto repaired = repaired_database_text(observed->content->getBuffer());
    auto damaged = [&](llvm::StringRef value) {
        return repaired && value.contains("\xEF\xBF\xBD");
    };
    simdjson::padded_string json_buf(repaired ? llvm::StringRef(*repaired)
                                              : observed->content->getBuffer());
    simdjson::ondemand::parser json_parser;
    simdjson::ondemand::document doc;
    if(auto error = json_parser.iterate(json_buf).get(doc)) {
        LOG_ERROR("Failed to parse compilation database from {}: {}",
                  path,
                  simdjson::error_message(error));
        return std::nullopt;
    }

    simdjson::ondemand::array arr;
    if(auto error = doc.get_array().get(arr)) {
        // PowerShell's ConvertTo-Json writes a one-entry array as the entry.
        simdjson::dom::parser checker;
        simdjson::dom::object single;
        if(!checker.parse(json_buf).get(single) && single["file"].is_string()) {
            LOG_ERROR(
                "Invalid compilation database format in {}: the root is one entry, not an "
                "array of them (PowerShell's ConvertTo-Json writes a single entry so); wrap it "
                "in [ ].",
                path);
            return std::nullopt;
        }
        LOG_ERROR("Invalid compilation database format in {}: root element must be an array.",
                  path);
        return std::nullopt;
    }

    // Parse into a local vector and only swap it in at the end: a file that
    // fails to read or parse at the top level leaves the loaded entries
    // intact, so reload_and_diff() reports no change instead of dropping
    // every file. A file truncated mid-array is NOT caught here (the
    // entries before the cut still swap in) — the CDB poll's two-tick
    // debounce is what keeps half-written files from being read.
    std::vector<CompilationEntry> new_entries;
    Normalizer normalizer{.commands = command_table, .files = file_table, .inputs = &source.inputs};

    // A database written inside Cygwin or MSYS2 (CMake run from their shell)
    // spells its paths POSIX-style: the mount table of the runtime on PATH,
    // found at the first such entry, maps the entries so spelled.
    std::optional<MountTable> table;

    llvm::StringMap<Spelling> working_dirs;
    std::uint32_t index = 0;
    for(auto element: arr) {
        auto skip = llvm::make_scope_exit([&] { index += 1; });

        simdjson::ondemand::object obj;
        if(element.get_object().get(obj)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: "
                "item is not an object.",
                path,
                index);
            continue;
        }

        std::string_view dir_sv, file_sv;
        if(obj["directory"].get_string().get(dir_sv)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: "
                "'directory' key is missing.",
                path,
                index);
            continue;
        }

        if(obj["file"].get_string().get(file_sv)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: "
                "'file' key is missing.",
                path,
                index);
            continue;
        }

        llvm::BumpPtrAllocator local;
        llvm::StringSaver saver(local);
        llvm::SmallVector<const char*, 32> args;
        std::optional<llvm::StringRef> command;
        simdjson::ondemand::array args_arr;
        if(!obj["arguments"].get_array().get(args_arr)) {
            bool malformed = false;
            for(auto arg_val: args_arr) {
                std::string_view sv;
                if(arg_val.get_string().get(sv)) {
                    malformed = true;
                    break;
                }
                args.push_back(saver.save(llvm::StringRef(sv.data(), sv.size())).data());
            }
            if(malformed || args.empty()) {
                continue;
            }
        } else {
            std::string_view cmd_sv;
            if(obj["command"].get_string().get(cmd_sv)) {
                LOG_ERROR(
                    "Invalid compilation database in {}. Skipping item at index {}: "
                    "neither 'arguments' nor 'command' key is present.",
                    path,
                    index);
                continue;
            }
            command = llvm::StringRef(cmd_sv.data(), cmd_sv.size());
        }

        llvm::StringRef file_ref(file_sv.data(), file_sv.size());
        if(damaged(dir_sv) || damaged(file_ref) ||
           (command ? damaged(*command) : ranges::any_of(args, damaged))) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {} ({}): "
                "it holds bytes that are not UTF-8.",
                path,
                index,
                file_ref);
            continue;
        }

        llvm::StringRef dir_ref(dir_sv.data(), dir_sv.size());
        normalizer.mounts = nullptr;
        if(is_posix_path(dir_ref)) {
            if(!table) {
                // Another runtime's table maps the directory under its own
                // root; another of one kind, Git for Windows' MSYS2 beside
                // MSYS2, maps a drive's paths alike but lacks the compiler.
                std::vector<std::string> written = {dir_ref.str()};
                auto tokens = command ? tokenize_sh_command(*command, saver) : args;
                auto compiler = wrapper_prefix_len(tokens);
                if(compiler < tokens.size() && is_posix_path(tokens[compiler])) {
                    written.emplace_back(tokens[compiler]);
                }
                auto [found] = kota::run(find_mount_table({}, std::move(written)));
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
        auto [known, fresh] = working_dirs.try_emplace(directory.str());
        if(fresh) {
            known->second = Spelling(CanonicalPath(directory));
        }
        auto& working = known->second;

        // Some build systems emit non-C-family files (e.g. .rc, .asm, .S, .def)
        // into compile_commands.json: skipped, unless the command's -x
        // says what clang does not know the extension for (a `.ixx`
        // module interface); one clang knows as another language stays
        // skipped, as the server and inspect skip it.
        bool c_family = is_c_family_file(file_ref);

        auto file = spelled(file_ref, directory, normalizer.mounts);
        auto path_id = file_table.intern_spelled(file);
        llvm::StringRef spelling = file.str() != llvm::StringRef(file_table.resolve(path_id))
                                       ? command_table.intern(file.str())
                                       : llvm::StringRef();

        auto normalized = command ? normalizer.normalize(working, path_id, *command)
                                  : normalizer.normalize(working, path_id, args);

        if(!normalized || is_other_language_path(file_ref) ||
           (!c_family && !command_table.forces_c_family(*normalized))) {
            continue;
        }
        new_entries.push_back({.file = path_id,
                               .command = *normalized,
                               .source = id,
                               .ordinal = index,
                               .spelling = spelling});
    }

    auto count = new_entries.size();
    source.entries = std::move(new_entries);
    source.loaded = true;
    source.present = true;
    auto responses = ranges::subrange(source.inputs.begin() + 1, source.inputs.end());
    ranges::sort(responses, {}, &LoadInput::file);
    auto duplicates = ranges::unique(responses, {}, &LoadInput::file);
    source.inputs.erase(duplicates.begin(), duplicates.end());
    rebuild_entry_list();
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

llvm::StringRef CompilationDatabase::spelling(Fid file) const {
    for(auto& entry: candidate_entries(file)) {
        if(!entry.spelling.empty()) {
            return entry.spelling;
        }
    }
    return file_table.resolve(file);
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
    auto& source = *anonymous;
    CompilationEntry entry{.file = file,
                           .command = *normalized,
                           .source = id,
                           .ordinal = static_cast<std::uint32_t>(source.entries.size())};
    source.entries.push_back(entry);
    rebuild_entry_list();
    return entry;
}

std::optional<CompilationEntry>
    CompilationDatabase::add_command(llvm::StringRef directory,
                                     llvm::StringRef file,
                                     llvm::ArrayRef<const char*> arguments) {
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
