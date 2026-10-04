#pragma once

/// What the commands reading module facts out of the index share: the
/// scope and partition flags, and the facts they select.

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "analysis/module_graph.h"
#include "analysis/wrapping.h"
#include "driver/query_support.h"

#include "llvm/ADT/StringRef.h"

namespace clice::driver {

std::vector<std::string> comma_list(llvm::StringRef text);

/// libc++'s module sources in `directory`, none for an empty one.
std::expected<std::optional<analysis::StdModules>, std::string> read_std(llvm::StringRef directory);

/// The partition file at `path`, if any, added to `spec`: modules claimed
/// by globs, first match wins. A module's `provides` names the module
/// exporting its names, std.compat for the C library, which `libcxx`
/// resolves.
std::expected<analysis::PartitionSpec, std::string>
    read_partition(analysis::PartitionSpec spec,
                   llvm::StringRef path,
                   const std::optional<analysis::StdModules>& libcxx);

/// The facts of the files `scope`'s comma-separated globs select out of the
/// loaded index (default: the workspace's own files outside dot
/// directories), and the workspace root.
struct LoadedFacts {
    std::string root;
    analysis::Facts facts;
};

std::expected<LoadedFacts, Failure> load_facts(llvm::StringRef workspace,
                                               llvm::StringRef configuration,
                                               llvm::StringRef scope);

}  // namespace clice::driver
