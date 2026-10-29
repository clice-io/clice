module clice:feature.signature_help;

import :compile.compilation;
import :feature.documentation;
import :support.prelude;

namespace clice::feature {

/// Corresponds to the `[signature_help]` section in clice.toml.
struct SignatureHelpOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Show the called function's documentation comment.")
    <bool> documentation = true;
};

/// `selected` is the label of the signature the client shows active: it
/// stays active while it is listed.
auto signature_help(CompilationParams params,
                    const SignatureHelpOptions& options,
                    const DocumentationOptions& documentation,
                    llvm::StringRef selected = {}) -> protocol::SignatureHelp;

/// Whether the code at `offset` of `text`, lexed in `dialect`, can hold a
/// call: a directive's name, a header name and the arguments the
/// preprocessor does not expand cannot — `#include <` and `#define MAX(a, `
/// ask for no signature, `#if CHECK(1, ` and `#include PICK(1, ` do.
bool can_hold_call(llvm::StringRef text, std::uint32_t offset, const clang::LangOptions& dialect);

}  // namespace clice::feature
