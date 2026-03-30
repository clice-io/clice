#pragma once

#include <cstddef>
#include <string>
#include <vector>

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

}  // namespace clice::testing
