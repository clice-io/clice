#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "server/editor_context.h"
#include "server/extension.h"
#include "server/session.h"

#include "kota/async/async.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ASTFamily;
struct SessionStore;

namespace protocol = kota::ipc::protocol;

/// How many errors name what an includer would have provided: an
/// undeclared name, a missing member or template of a namespace, a call to
/// an undeclared C function, an #error guarding against a standalone
/// compile, a conditional the includer closes. A typo raises the same
/// ones; the self-containment trial runs when a header opens or its
/// inputs change, never per edit, so one costs at most a compile under
/// the includer's context.
std::size_t missing_context_errors(llvm::ArrayRef<protocol::Diagnostic> diagnostics);

/// The editor-facing context protocol (clice/queryContext, currentContext,
/// switchContext) and session-coupled maintenance of context choices. The
/// domain state lives in EditorContext; this service drives it with LSP
/// types and session knowledge, which the editor context deliberately
/// knows nothing about.
struct ContextService {
    Project& project;
    EditorContext& editor;
    ASTFamily& ast;

    /// The compilation contexts this project offers a file (clice/queryContext):
    /// its host sources and the file's own CDB configurations.
    std::vector<ext::ContextItem> contexts(Fid path_id);

    /// clice/currentContext: describe the file's currently active context.
    ext::CurrentContextResult current_context(const Session* session);

    /// clice/switchContext: pin a host source or CDB entry as the file's
    /// compilation context and persist the choice across sessions. When
    /// persistence is available, success is acknowledged only once the
    /// choice is durably committed: the request marks the contexts blob
    /// dirty and awaits the write pipeline's ticket. Sessions without a
    /// writable database (read-only, caching disabled, open failure) apply
    /// the choice in memory and acknowledge immediately.
    kota::task<ext::SwitchContextResult> switch_context(Fid path_id,
                                                        Session* session,
                                                        Fid context_path_id,
                                                        const ext::SwitchContextParams& params);

    /// clice/listConfigurations: the configuration menu with the running,
    /// persisted and default names.
    ext::ListConfigurationsResult list_configurations() const;

    /// clice/switchConfiguration: persist `name` as the configuration the
    /// next server start activates; the running one is unchanged. Fails
    /// for a name no rule declares, while `pinned` (the command line's
    /// `--configuration`, when it names a declared tag) owns the choice,
    /// and when the selection cannot be persisted.
    ext::SwitchConfigurationResult switch_configuration(llvm::StringRef name,
                                                        llvm::StringRef pinned);

    /// Drop active context choices whose include edge no longer exists. A
    /// stale choice suppresses automatic host resolution, so it would strand
    /// the header on the fallback command (or silently pin its command hash
    /// to a different host). Expects the include graph to be current (the
    /// caller rescans on save). Returns whether any persisted choice was
    /// removed, i.e. whether the cache snapshot needs saving.
    bool drop_orphaned_choices(SessionStore& sessions);
};

}  // namespace clice
