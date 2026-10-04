#pragma once

/// What the commands answering from the persisted index share: the JSON
/// envelope on stdout, the symbol locator their flags spell, and the
/// --fresh refresh.

#include <expected>
#include <format>
#include <string>
#include <vector>

#include "driver/driver.h"
#include "index/symbol_query.h"
#include "vfs/file_system.h"
#include "vfs/path.h"
#include "worker/serialize.h"

#include "kota/ipc/codec/json.h"
#include "llvm/ADT/StringRef.h"

namespace clice::driver {

/// One answer on stdout: `{"result": ..., "stale": [...]}`, the files
/// whose rows were withheld because their content moved on from the
/// index (or that the index never held) listed so the reader knows what
/// the answer lacks.
template <typename T>
struct Answer {
    T result;
    std::vector<std::string> stale;
};

/// `{"error": "...", "stale": [...]}` on stdout, exit code 1 (2 for
/// arguments that do not parse): what kept the question from being
/// answered, and the files withheld on the way — a symbol not found may
/// sit in one of them.
struct Failure {
    std::string error;
    std::vector<std::string> stale;
};

template <typename T>
std::string render_json(const T& value) {
    return to_client_json(value, "null");
}

template <typename T>
void print_json(const T& value) {
    driver::println("{}", render_json(value));
}

/// The symbol locator the flags of `opts` spell, as a name query:
/// `--symbol` an id (anchored at `--path`), `--name` a name query narrowed
/// to `--path`, or `--path` and `--line` a place. The path must be a file;
/// one the index has no rows for is noted as unindexed by the command.
template <typename Options>
std::expected<index::SymbolQuery, std::string> locator_of(const Options& opts,
                                                          llvm::StringRef absolute) {
    if(opts.line && *opts.line <= 0) {
        return std::unexpected("line must be positive");
    }
    if(opts.path && !vfs::is_file(absolute)) {
        return std::unexpected(std::format("no such file: {}", std::string_view(absolute)));
    }
    if(opts.symbol) {
        auto parsed = index::SymbolQuery::parse(*opts.symbol);
        if(!parsed || !parsed->handle) {
            return std::unexpected(std::format("invalid symbol id: {}", *opts.symbol));
        }
        if(opts.path) {
            parsed->paths.emplace_back(absolute);
        }
        return std::move(*parsed);
    }
    // The path is a literal, never query text: it joins the parsed query
    // as the filter or the place it stands for.
    if(opts.name) {
        auto parsed = index::SymbolQuery::parse(*opts.name);
        if(!parsed) {
            return std::unexpected(parsed.error());
        }
        if(opts.path) {
            parsed->paths.emplace_back(absolute);
        }
        return std::move(*parsed);
    }
    if(opts.path && opts.line) {
        index::SymbolQuery query;
        query.position = {.path = std::string(absolute), .line = *opts.line};
        return query;
    }
    return std::unexpected("name a symbol with --name, --symbol, or --path and --line");
}

/// Bring the index up to date with the disk before a --fresh answer; the
/// units that failed to index, or why the refresh could not run.
std::expected<std::vector<std::string>, std::string> refresh(const Spelling& workspace,
                                                             llvm::StringRef configuration,
                                                             const char* self_path);

}  // namespace clice::driver
