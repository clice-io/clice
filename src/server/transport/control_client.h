#pragma once

#include <expected>
#include <string>
#include <vector>

#include "index/writer_lock.h"
#include "server/protocol/control.h"

#include "llvm/ADT/StringRef.h"

namespace clice::control {

/// Ask the serving writer at `endpoint` to sweep the build and wait for
/// the rows to be persisted. Runs to completion on an event loop of its
/// own; a refused connection or a server that goes away mid-request is
/// the error.
std::expected<IndexResult, std::string> request_index(const index::ServerEndpoint& endpoint,
                                                      llvm::StringRef configuration);

std::expected<StatusResult, std::string> request_status(const index::ServerEndpoint& endpoint);

}  // namespace clice::control
