#pragma once

#include "Support/Support.h"

namespace clice::config {

/// Read the config file, call when the program starts.
void load(llvm::StringRef execute, llvm::StringRef filename);

/// Initialize the config, replace all predefined variables in the config file.
/// called in `Server::initialize`.
void init(std::string_view workplace);

struct ServerOptions {
    std::vector<std::string> compile_commands_dirs;
};

struct CacheOptions {
    std::string dir;
    uint32_t limit = 0;
};

struct IndexOptions {
    std::string dir;
    bool implicitInstantiation = true;
};

struct Rule {
    std::string pattern;
    std::vector<std::string> append;
    std::vector<std::string> remove;
    std::string readonly;
    std::string header;
    std::vector<std::string> context;
};

extern llvm::StringRef version;
extern llvm::StringRef binary;
extern llvm::StringRef llvm_version;
extern llvm::StringRef workspace;

extern const ServerOptions& server;
extern const CacheOptions& cache;
extern const IndexOptions& index;
extern llvm::ArrayRef<Rule> rules;

};  // namespace clice::config

