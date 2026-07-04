#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command/command.h"
#include "server/protocol/extension.h"
#include "server/session/session.h"
#include "server/workspace/workspace.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Domain logic for compilation contexts of header files.
///
/// A header without its own compilation database entry borrows a host
/// source's command through the include graph. ContextResolver owns that
/// resolution and synthesis (prefix/suffix/self-snapshot files restoring the
/// includer's preprocessor state), the editor-facing context protocol
/// extension (clice/queryContext, currentContext, switchContext), and
/// restoring a persisted context choice into a session on didOpen.
///
/// Owns no state of its own — reads project-wide truth from Workspace and
/// per-file state from Session, mirroring Compiler's stateless design. Used
/// by Compiler (compile-argument resolution) and LSPClient (protocol
/// handlers) through a shared reference.
class ContextResolver {
public:
    explicit ContextResolver(Workspace& workspace) : workspace(workspace) {}

    /// Fill compile arguments for a header from a host source's command found
    /// through the include graph, synthesizing a preamble prefix/suffix when
    /// the header needs includer context. Returns false when no usable host
    /// context exists. @param session may be null (background indexing).
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  Session* session);

    /// Restore a context choice persisted from an earlier run into the
    /// session, validating it against the current CDB and include graph and
    /// dropping it when stale.
    void restore_saved_context(Session& session);

    /// clice/queryContext: list the compilation contexts (host sources and
    /// the file's own CDB configurations) available for a file, paginated.
    ext::QueryContextResult query_contexts(llvm::StringRef path,
                                           std::uint32_t path_id,
                                           const ext::QueryContextParams& params);

    /// clice/currentContext: describe the file's currently active context.
    ext::CurrentContextResult current_context(llvm::StringRef path,
                                              const Session* session,
                                              const ext::CurrentContextParams& params);

    /// clice/switchContext: pin a host source or CDB entry as the file's
    /// compilation context and persist the choice across sessions.
    ext::SwitchContextResult switch_context(llvm::StringRef path,
                                            std::uint32_t path_id,
                                            Session* session,
                                            llvm::StringRef context_path,
                                            std::uint32_t context_path_id,
                                            const ext::SwitchContextParams& params);

private:
    std::optional<HeaderContext> resolve_header_context(std::uint32_t header_path_id,
                                                        Session* session,
                                                        bool synthesize);

    Workspace& workspace;
};

}  // namespace clice
