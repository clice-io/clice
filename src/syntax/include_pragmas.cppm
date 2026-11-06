module clice:syntax.include_pragmas;

import :support.prelude;

namespace clice {

/// What a header's include-what-you-use pragmas (`// IWYU pragma: ...`) say
/// of including it and the headers it includes.
struct IncludePragmas {
    /// `private`: no file is to include the header itself.
    bool is_private = false;

    /// `private, include "x.h"`: the header to include instead, as written,
    /// its delimiters with it.
    std::string include_instead;

    /// The header names, as written without their delimiters, of the
    /// includes an `export` marks, the pragma on the include's line, or a
    /// `begin_exports` … `end_exports` pair encloses: including the header
    /// is including those.
    std::vector<std::string> exports;

    /// The header names, as written without their delimiters, of the
    /// includes a `keep` marks, the pragma on the include's line, or a
    /// `begin_keep` … `end_keep` pair encloses: they stay whether used or
    /// not.
    std::vector<std::string> keeps;
};

IncludePragmas include_pragmas(llvm::StringRef text, const clang::LangOptions& dialect);

}  // namespace clice
