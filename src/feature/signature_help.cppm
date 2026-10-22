module;

#include "modules/prelude.h"

module clice:feature.signature_help;

import :compile.compilation;
import :feature.documentation;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

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

}  // namespace clice::feature
