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

struct SessionStore;

namespace protocol = kota::ipc::protocol;

/// Where the compile command for a file came from. Anything other than
/// CDBExact means the command was guessed to some degree, which is why
/// diagnostics produced with it may deserve a guidance note (see
/// format_diagnostics).
enum class CommandSource : std::uint8_t {
    /// Direct compilation database entry for the file.
    CDBExact,
    /// Header compiled in the context of a host source found through the
    /// include graph (automatic or via clice/switchContext).
    IncludeGraph,
    /// Reserved for command transfer heuristics (e.g. nearest CDB entry);
    /// no producer yet.
    Inferred,
    /// Synthesized default command — no CDB entry and no usable host source.
    Fallback,
};

/// Diagnostic codes that strictly indicate a missing includer context (as
/// opposed to ordinary in-progress typing errors). Deliberately narrow:
/// a false positive costs a pointless prefix synthesis, a false negative
/// just leaves the header in trial mode.
bool indicates_missing_context(llvm::ArrayRef<protocol::Diagnostic> diagnostics);

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

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: CDB entry, header context through the include graph,
    /// and finally a synthesized fallback command — so it always succeeds.
    /// Emits a per-file decision log (tiers tried, tier hit, command hash).
    /// @param session  If non-null, used for header context resolution on open files.
    CommandSource resolve_command(llvm::StringRef path,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  Session* session = nullptr);

    /// Append the header context's suffix as one trailing #include line: the
    /// suffix content (everything after the include position along the chain)
    /// lives in its own file so features never see it, while the token stream
    /// still closes any braces the fragment is embedded in. The single extra
    /// line sits past the editor's EOF and is invisible to the client.
    void append_suffix_include(const Session& session, std::string& text);

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

    /// Drop active context choices whose include edge no longer exists. A
    /// stale choice suppresses automatic host resolution, so it would strand
    /// the header on the fallback command (or silently pin its command hash
    /// to a different host). Expects the include graph to be current (the
    /// caller rescans on save). Returns whether any persisted choice was
    /// removed, i.e. whether the cache snapshot needs saving.
    bool drop_orphaned_choices(SessionStore& sessions);

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
