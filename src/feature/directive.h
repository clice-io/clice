#pragma once

#include <cstdint>
#include <optional>

#include "syntax/token.h"

#include "llvm/ADT/StringRef.h"
#include "clang/Basic/LangOptions.h"

namespace clice::feature {

/// Find the filename-like argument of a preprocessor directive on the line
/// containing `offset`. The offset may point at the directive/operator or
/// inside its argument.
auto find_directive_argument(llvm::StringRef content,
                             std::uint32_t offset,
                             const clang::LangOptions* lang_opts)
    -> std::optional<LocalSourceRange>;

}  // namespace clice::feature
