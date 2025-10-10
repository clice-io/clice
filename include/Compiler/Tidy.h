#pragma once

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace clang {
class CompilerInstance;
}

namespace clice::tidy {

bool is_registered_tidy_check(llvm::StringRef check);
std::optional<bool> is_fast_tidy_check(llvm::StringRef check);

struct TidyParams {};

class ClangTidyChecker;

/// Configure to run clang-tidy on the given file.
std::unique_ptr<ClangTidyChecker> configure(clang::CompilerInstance& instance,
                                            const TidyParams& params);

}  // namespace clice::tidy
