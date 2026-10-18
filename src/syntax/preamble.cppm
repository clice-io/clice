module;

#include "modules/prelude.h"

module clice:syntax.preamble;

namespace clice {

/// The end of the preamble: the directives, and the global module
/// fragment's `module;`, that open the file.
std::uint32_t preamble_bound(llvm::StringRef content);

/// The part of the preamble up to `bound` that is finished: `bound` itself,
/// or the end of the line before the first header-name directive
/// (#include, #embed, ...) whose "" or <> is still open — the user is
/// typing it, and it belongs with the body until done.
std::uint32_t finished_preamble_bound(llvm::StringRef content, std::uint32_t bound);

/// Whether a directive of the preamble up to `bound` defines, undefines or
/// tests a macro: the macro and its uses then sit at positions of the file
/// itself.
bool preamble_uses_macros(llvm::StringRef content, std::uint32_t bound);

/// Whether the text up to `bound` guards itself with `#pragma once`: a
/// compile enters it once, so its other includes find it skipped.
bool has_pragma_once(llvm::StringRef content, std::uint32_t bound);

}  // namespace clice
