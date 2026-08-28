#pragma once

#include <vector>

#include "server/state/ast_projection.h"
#include "server/state/session.h"

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

}  // namespace clice
