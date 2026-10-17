module;

#include "modules/prelude.h"

module clice:compile.diagnostic;

import :syntax.token;

namespace clice {

enum class DiagnosticLevel : std::uint8_t {
    Note,
    Remark,
    Warning,
    Error,
    Fatal,
};

enum class DiagnosticSource : std::uint8_t {
    Clang,
    ClangTidy,
    /// clice's own, about a file as a whole; no compile reports one.
    Clice,
};

struct DiagnosticID {
    /// The diagnostic id value.
    std::uint32_t value;

    /// The level of this diagnostic.
    DiagnosticLevel level;

    /// The source of diagnostic.
    DiagnosticSource source;

    /// The clang-tidy check that reported it, or the compiler's warning
    /// option that controls it (`unused-variable`); empty for neither.
    llvm::StringRef name;

    /// The code an editor shows it by: the check, `-W` and the warning
    /// option, else clang's name for it (`err_undeclared_var_use`).
    std::string diagnostic_code() const;

    /// Whether this diagnostic reports a failure to read a prebuilt
    /// artifact (PCH/PCM) — clang's "AST Deserialization Issue" category.
    /// Messages in this family do not reliably carry the artifact path
    /// (e.g. "malformed or corrupted precompiled file: 'Blob ends too
    /// soon'"), so consumers that must blame a specific artifact combine
    /// this with a path match over the inputs they passed.
    bool is_deserialization_error() const;
};

struct Diagnostic {
    /// The diagnostic id.
    DiagnosticID id;

    /// Whether clang maps the diagnostic to an error by default; `level`
    /// also counts the warnings -Werror raises.
    bool error_by_default = false;

    /// The file location of this diagnostic.
    clang::FileID fid;

    /// The source range of this diagnostic(may be invalid, if this diagnostic
    /// is from command line. e.g. unknown command line argument).
    LocalSourceRange range;

    /// The error message of this diagnostic.
    std::string message;

    /// The fix its fix-it hints spell, replacements of the focus file's
    /// text applied together; empty when it has none, or one the focus
    /// file's text cannot express.
    std::vector<TextReplacement> fix;

    /// The notes clang attached to it, in order.
    std::vector<Diagnostic> notes;
};

}  // namespace clice
