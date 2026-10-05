#pragma once

#include <expected>
#include <string>
#include <vector>

#include "kota/async/async.h"

namespace clice {

/// Run `arguments[0]` to completion and hand back one of its output
/// streams: stdout when `capture_stdout`, else stderr. Fails when the
/// program cannot start or does not exit with code 0.
kota::task<std::expected<std::string, std::string>> execute(std::vector<std::string> arguments,
                                                            bool capture_stdout = false,
                                                            std::string cwd = {});

}  // namespace clice
