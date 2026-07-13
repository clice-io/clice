#pragma once

#include <string>

#include "syntax/token.h"

namespace clice::feature {

/// A resolved document link: the argument range of an include-like
/// directive (byte offsets in the containing file) and the absolute path
/// of the target file. Plain data — it serializes over the worker RPC and
/// the PCH's PreambleState blob as-is and becomes an LSP DocumentLink only
/// at the reply edge, where the session's line map does the conversion.
struct DocumentLink {
    LocalSourceRange range;
    std::string target;
};

}  // namespace clice::feature
