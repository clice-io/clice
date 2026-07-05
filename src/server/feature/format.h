#pragma once

#include <vector>

#include "server/session/session.h"

#include "kota/ipc/lsp/protocol.h"

namespace clice {

namespace protocol = kota::ipc::protocol;

/// Format a materialized compile output into publishable diagnostics:
/// deserialize the worker's raw diagnostics, drop phantom suffix-include
/// lines, and — when the compile command was not an exact CDB match and
/// the diagnostics contain file-not-found class errors — merge a file-top
/// guidance diagnostic explaining the inferred command. Formatting is
/// compile semantics; sending the result is the transport's job.
std::vector<protocol::Diagnostic> format_diagnostics(const CompileOutput& output);

/// Convert a compile output's inactive-region byte offsets into LSP ranges
/// using the session's line map.
std::vector<protocol::Range> format_inactive_regions(const Session& session,
                                                     const CompileOutput& output);

}  // namespace clice
