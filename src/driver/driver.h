#pragma once

// No textual std includes here: driver TUs import clice, and mixing textual
// libc++ headers with the module BMI trips abi_tag redeclaration errors on
// macOS. std names resolve through the stdlib wrapper exports.
#include "kota/deco/deco.h"

namespace clice::driver {

/// Each subcommand lives in its own translation unit under src/driver/ and
/// registers itself with the root commander through one of these builders.
/// `exit_code` is written by the matched handler when the command runs;
/// clice.cc owns the commander and the final process exit.

void add_serve(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_query(kota::deco::cli::SubCommander& root, int& exit_code);
void add_worker(kota::deco::cli::SubCommander& root, int& exit_code);
void add_index(kota::deco::cli::SubCommander& root, int& exit_code);
void add_doc(kota::deco::cli::SubCommander& root, int& exit_code);
void add_lint(kota::deco::cli::SubCommander& root, int& exit_code);
void add_format(kota::deco::cli::SubCommander& root, int& exit_code);

/// Set the global log level from a user-supplied string; complains and
/// returns false on an unknown level.
inline bool apply_log_level(const std::string& level_str) {
    auto level = logging::parse_level(level_str);
    if(!level) {
        std::println(stderr,
                     "unknown log level '{}', valid: trace, debug, info, warn, error, off",
                     level_str);
        return false;
    }
    logging::options.level = *level;
    return true;
}

template <typename Command>
void print_usage(Command& cmd) {
    std::ostringstream ss;
    cmd.usage(ss);
    std::print("{}", ss.str());
}

}  // namespace clice::driver
