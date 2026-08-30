#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "test/temp_dir.h"
#include "command/command.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice::testing {

struct CDBEntry {
    llvm::StringRef dir;
    std::string file;
    std::vector<std::string> extra_args;
};

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

/// Write a compile_commands.json into the temp dir and load it into the given CDB.
inline void write_cdb(TempDir& tmp, CompilationDatabase& cdb, llvm::StringRef json_content) {
    tmp.touch("compile_commands.json", json_content);
    cdb.load(tmp.path("compile_commands.json"));
}

/// Rules-applied driver-level render of a file's default candidate, with
/// the injected resource dir stripped so tests can assert exact argv.
/// A file without candidates renders empty — the caller's assertion then
/// fails readably instead of the front() of an empty list crashing.
inline std::vector<const char*> render_entry(CompilationDatabase& cdb,
                                             llvm::StringRef file,
                                             const CommandOptions& options = {}) {
    auto candidates = cdb.candidate_entries(file);
    if(candidates.empty()) {
        return {};
    }
    auto& entry = candidates.front();
    auto applied = cdb.apply_rules(entry.config, options);
    CommandRef ref{entry.file, applied, cdb.input_kind(applied, file), CommandSource::CDBExact};
    auto argv = cdb.render_driver(ref);
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(llvm::StringRef(argv[i]) == "-resource-dir") {
            argv.erase(argv.begin() + i, argv.begin() + i + 2);
            break;
        }
    }
    return argv;
}

}  // namespace clice::testing
