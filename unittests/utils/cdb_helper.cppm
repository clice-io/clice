module clice:unittests.utils.cdb_helper;

import :command.database;
import :command.edit;
import :command.table;
import :command.toolchain;
import :support.prelude;
import :syntax.dependency_graph;
import :unittests.utils.temp_dir;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {

struct CDBEntry {
    llvm::StringRef dir;
    std::string file;
    std::vector<std::string> extra_args;
};

/// A file the tests' hand-added commands compile: they run in /fake, and a
/// render names the input by its identity.
inline std::string fake(llvm::StringRef name) {
    return CanonicalPath(Spelling(name, Spelling::absolute("/fake"))).str();
}

/// Escape backslashes and quotes for JSON string values.
inline std::string json_escape(llvm::StringRef s) {
    std::string result;
    result.reserve(s.size());
    for(char c: s) {
        if(c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

/// Build a compile_commands.json array from entries.
/// Uses "arguments" array form to avoid platform-specific tokenization issues.
inline std::string build_cdb_json(llvm::ArrayRef<CDBEntry> entries) {
    std::string json = "[\n";
    for(std::size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if(i > 0) {
            json += ",\n";
        }
        json += R"(  {"directory": ")";
        json += json_escape(e.dir);
        json += R"(", "file": ")";
        json += json_escape(e.file);
        json += R"(", "arguments": ["clang++", "-std=c++20")";
        for(auto& arg: e.extra_args) {
            json += R"(, ")";
            json += json_escape(arg);
            json += R"(")";
        }
        json += R"(, ")";
        json += json_escape(e.file);
        json += R"("]})";
    }
    json += "\n]";
    return json;
}

/// The command the database lists for `entry`, its language read off it.
inline CommandRef database_command(CompilationDatabase& cdb, const CompilationEntry& entry) {
    return {entry.file,
            entry.command,
            cdb.commands().input_kind(entry.command, cdb.files().resolve(entry.file)),
            CommandSource::Database};
}

/// Write a compile_commands.json into the temp dir and load it into the given CDB.
inline void write_cdb(TempDir& tmp, CompilationDatabase& cdb, llvm::StringRef json_content) {
    tmp.touch("compile_commands.json", json_content);
    cdb.load(tmp.path("compile_commands.json"));
}

/// List the file at the absolute `path` in `cdb` with a command of its own,
/// a unit of the build: the background index books only those.
inline Fid list_unit(CompilationDatabase& cdb, llvm::StringRef path) {
    return cdb
        .add_command(path::parent(path), path, llvm::StringRef(std::format("clang++ -c {}", path)))
        ->file;
}

/// Whether one argument contains `needle` once its separators are
/// normalized: include paths absolutize with the native separator, so the
/// joined line cannot be searched for a `/`-spelled path.
inline bool has_arg(llvm::ArrayRef<const char*> argv, llvm::StringRef needle) {
    return ranges::any_of(argv, [&](const char* arg) {
        std::string slashed(arg);
        path::to_slash(slashed);
        return llvm::StringRef(slashed).contains(needle);
    });
}

/// Rules-applied driver-level render of a file's default candidate, with
/// the injected resource dir stripped so tests can assert exact argv.
/// A file without candidates renders empty — the caller's assertion then
/// fails readably instead of the front() of an empty list crashing.
inline std::vector<const char*> render_entry(CompilationDatabase& cdb,
                                             llvm::StringRef file,
                                             const CommandEdits& options = {}) {
    auto candidates = cdb.candidate_entries(file);
    if(candidates.empty()) {
        return {};
    }
    auto& entry = candidates.front();
    auto& commands = cdb.commands();
    auto applied = apply_edits(commands, cdb.files(), entry.command, options);
    CommandRef ref{entry.file,
                   applied,
                   commands.input_kind(applied, file),
                   CommandSource::Database};
    auto argv = commands.render_driver(ref, cdb.spelling(ref.file));
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(llvm::StringRef(argv[i]) == "-resource-dir") {
            argv.erase(argv.begin() + i, argv.begin() + i + 2);
            break;
        }
    }
    return argv;
}

/// Scan every entry of the database under its own command: the shape of a
/// test that owns no configuration.
inline ScanReport scan_all(CompilationDatabase& cdb, Toolchain& toolchain, DependencyGraph& graph) {
    auto units = ranges::to<llvm::SmallVector<CommandRef>>(
        views::transform(cdb.entries(), [&](auto& entry) { return database_command(cdb, entry); }));
    return scan_dependency_graph(cdb, toolchain, graph, units);
}

}  // namespace clice::testing
