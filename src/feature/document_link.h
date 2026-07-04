#pragma once

#include <string>

#include "kota/ipc/lsp/protocol.h"

namespace clice::feature {

/// A resolved document link: the argument range of an include-like
/// directive and the absolute path of the target file. Plain data — it
/// serializes over the worker RPC as-is and becomes an LSP DocumentLink
/// only at the reply edge.
struct DocumentLink {
    kota::ipc::protocol::Range range;
    std::string target;
};

}  // namespace clice::feature
