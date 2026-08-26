#pragma once

#include <cstdint>

#include "sched/context.h"
#include "server/protocol/extension.h"
#include "server/state/session.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ASTFamily;
struct SessionStore;

namespace protocol = kota::ipc::protocol;

/// Diagnostic codes that strictly indicate a missing includer context (as
/// opposed to ordinary in-progress typing errors). Deliberately narrow:
/// a false positive costs a pointless prefix synthesis, a false negative
/// just leaves the header in trial mode.
bool indicates_missing_context(llvm::ArrayRef<protocol::Diagnostic> diagnostics);

/// The editor-facing context protocol (clice/queryContext, currentContext,
/// switchContext) and session-coupled maintenance of context choices. The
/// domain state lives in ContextResolver; this service drives it with LSP
/// types and session knowledge, which the resolver deliberately knows
/// nothing about.
struct ContextService {
    Workspace& workspace;
    ContextResolver& resolver;
    ASTFamily& ast;

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

    /// Drop active context choices whose include edge no longer exists. A
    /// stale choice suppresses automatic host resolution, so it would strand
    /// the header on the fallback command (or silently pin its command hash
    /// to a different host). Expects the include graph to be current (the
    /// caller rescans on save). Returns whether any persisted choice was
    /// removed, i.e. whether the cache snapshot needs saving.
    bool drop_orphaned_choices(SessionStore& sessions);
};

}  // namespace clice
