module clice:server.context_service;

import :server.ast_family;
import :server.editor_context;
import :server.extension;
import :server.include_trees;
import :server.session;
import :server.session_store;
import :support.prelude;

namespace clice {

/// The commands of the source actions switching what a document compiles
/// under (context_actions, configuration_actions), each standing for the
/// request of the same name with its parameters as the one argument.
/// Named apart from the VS Code extension's commands, which the client
/// library registers them beside.
constexpr inline std::string_view switch_context_command = "clice.lsp.switchContext";
constexpr inline std::string_view reset_context_command = "clice.lsp.resetContext";
constexpr inline std::string_view switch_configuration_command = "clice.lsp.switchConfiguration";

/// The kinds of those source actions.
constexpr inline std::string_view context_action_kind = "source.clice.context";
constexpr inline std::string_view configuration_action_kind = "source.clice.configuration";

/// Source actions switching the context the document at `uri` compiles
/// under, for an editor whose only UI for it is its code action menu (Zed,
/// Helix): every other context of the listing's first page, and the
/// automatic one while the user's choice (`current`) is in force; none
/// when the listing offers no choice.
std::vector<protocol::CodeAction> context_actions(llvm::StringRef uri,
                                                  const ext::CurrentContextResult& current,
                                                  const ext::QueryContextResult& listing);

/// Source actions choosing every other build configuration for the next
/// start.
std::vector<protocol::CodeAction> configuration_actions(llvm::StringRef uri,
                                                        const ext::ListConfigurationsResult& list);

/// The editor-facing context protocol (clice/queryContext, currentContext,
/// switchContext) and session-coupled maintenance of context choices. The
/// domain state lives in EditorContext; this service drives it with LSP
/// types and session knowledge, which the editor context deliberately
/// knows nothing about.
struct ContextService {
    Project& project;
    EditorContext& editor;
    ASTFamily& ast;
    IncludeTrees& trees;

    /// The compilation contexts this project offers a file (clice/queryContext):
    /// its host sources and the file's own CDB configurations.
    std::vector<ext::ContextItem> contexts(Fid path_id);

    /// Take the include tree of `host` that contexts() counts the places
    /// its compile enters `header` by.
    kota::task<> take_tree(Fid header, Fid host);

    /// clice/currentContext: the listing's item for the context the file
    /// compiles under, whether the user chose it, and its command.
    kota::task<ext::CurrentContextResult> current_context(const Session* session);

    /// clice/switchContext: pin a host source or CDB entry — one contexts()
    /// lists — as the file's compilation context and persist the choice
    /// across sessions. When persistence is available, success is
    /// acknowledged only once the choice is durably committed: the request
    /// marks the context choices dirty and awaits a save covering the
    /// mark. Sessions without a writable database (read-only, caching
    /// disabled, open failure) apply the choice in memory and acknowledge
    /// immediately.
    kota::task<ext::SwitchContextResult> switch_context(Session& session,
                                                        Fid context_path_id,
                                                        const ext::SwitchContextParams& params);

    /// Start the document over under another context: nothing it earned
    /// under the one it leaves — the compile in flight and what it built —
    /// carries over.
    void leave_context(Session& session);

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
