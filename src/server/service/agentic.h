#pragma once

#include <string>

#include "kota/deco/deco.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace deco = kota::deco;

struct QueryOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate, help = "Server host", required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Server port (required)",
           required = false)
    <int> port;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Query method (compileCommand, symbolSearch, definition, references, "
                  "documentSymbols, readSymbol, callGraph, typeHierarchy, projectFiles, "
                  "fileDeps, impactAnalysis, status, shutdown)",
           required = false)
    <std::string> method = "compileCommand";

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "File path for queries",
           required = false)
    <std::string> path;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate, help = "Symbol name", required = false)
    <std::string> name;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Search query string",
           required = false)
    <std::string> query;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Line number for position-based lookup",
           required = false)
    <int> line;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Direction: callers/callees or supertypes/subtypes",
           required = false)
    <std::string> direction;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level = "info";
};

int run_agentic_mode(const QueryOptions& opts);

}  // namespace clice
