module;

#include "modules/prelude.h"

module clice:server.format;

import :feature.feature;
import :server.compile_result;
import :server.session;

namespace clice {

/// The diagnostics a compile output shows on its file: those on the
/// file's own lines (a borrowed context appends the includer's remainder
/// past them) and not suppressed by `options`, behind a file-top warning
/// when they can be explained by a guessed command or an includer context
/// that could not be rebuilt. Sending them is the transport's job.
std::vector<feature::FileDiagnostic> format_diagnostics(const CompileOutput& output,
                                                        const feature::DiagnosticsOptions& options);

}  // namespace clice
