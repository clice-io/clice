module clice:server.extension;

import :support.prelude;

namespace clice::ext {

struct ContextItem {
    std::string label;
    std::string description;

    /// Host source file (header contexts) or the file itself (source
    /// compile configurations).
    std::string uri;

    /// For header contexts: which place the host's compile enters the
    /// header this context represents (0-based, in the order it does).
    /// Present only when it enters the header more than once.
    std::optional<std::uint32_t> occurrence;

    /// For source compile configurations: canonical hash identifying the
    /// CDB entry. Pass it back in switchContext to select this entry.
    std::optional<std::string> command_hash;
};

struct QueryContextParams {
    std::string uri;
    std::optional<int> offset;
};

struct QueryContextResult {
    std::vector<ContextItem> contexts;
    int total = 0;

    /// Workspace state generation these results were computed against.
    /// Pass it back in switchContext to detect stale listings.
    std::uint64_t epoch = 0;
};

/// The command line an open document compiles with, as its database or
/// rule writes it with the rules of clice.toml applied, naming the
/// document, and where it comes from.
struct CompileCommand {
    /// Where the command comes from: "database", an entry of a compilation
    /// database; "host", the command of a source including the header;
    /// "default", the default_command of a rule; "borrowed", the command
    /// of a unit nearby; "fallback", clice's own.
    std::string source;

    /// The file whose command it borrows: a header's host, the unit a
    /// borrowed command comes from when the source is not one of the build
    /// yet (a saved source joined it with the command it borrowed).
    std::optional<std::string> from;

    std::string directory;
    std::vector<std::string> arguments;

    /// Why the compiler of the command could not be queried, which left the
    /// arguments clang's own reading of the command; absent when it was.
    std::optional<std::string> query_failure;
};

struct CurrentContextParams {
    std::string uri;
};

struct CurrentContextResult {
    /// The context the file compiles under, as queryContext lists it:
    /// the user's choice, else the one picked automatically; empty when
    /// the file borrows no context and has no entry of its own.
    std::optional<ContextItem> context;

    /// Whether no choice of the user's is in force.
    bool automatic = true;

    /// The user's choice while it does not hold — its host left the build or
    /// no longer includes the file, or its command is gone: it waits, and
    /// applies again once it holds. Meanwhile the file compiles in the
    /// automatic context.
    std::optional<ContextItem> inactive_choice;

    /// The command the document compiles with; absent when it is not open.
    std::optional<CompileCommand> command;

    /// The listing generation it answers under (QueryContextResult::epoch):
    /// a listing of another epoch is out of date.
    std::uint64_t epoch = 0;

    /// Whether the file's answers show, beside its own context, the code
    /// its other contexts compile (clice/showOtherContexts).
    bool other_contexts = true;
};

/// clice/resetContext: drop the user's choice, back to the automatic one.
struct ResetContextParams {
    std::string uri;
};

/// clice/showOtherContexts: whether a file's answers show, beside its own
/// context, the code its other contexts compile — the default —, or its
/// own context alone. Kept while the server runs.
struct ShowOtherContextsParams {
    std::string uri;
    bool show = true;
};

struct SwitchContextParams {
    std::string uri;
    std::string context_uri;

    /// Include occurrence to choose (header contexts, 0-based).
    std::optional<std::uint32_t> occurrence;

    /// Command hash of the entry to choose (source files with multiple
    /// compile commands).
    std::optional<std::string> command_hash;

    /// Epoch of the queryContext result this choice came from. When set
    /// and the workspace has changed since, the switch is rejected with
    /// stale = true and the client should re-query.
    std::optional<std::uint64_t> epoch;
};

struct SwitchContextResult {
    bool success = false;

    /// The request referenced an outdated queryContext listing.
    bool stale = false;
};

/// clice/listConfigurations: the build configuration menu and the names
/// the selection layers hold, of the project serving `uri` — without one,
/// of the first project over a folder.
struct ListConfigurationsParams {
    std::optional<std::string> uri;
};

/// The build the links in the active configuration's database paths
/// resolve to (rules with `resolve_symlinks`).
struct LinkedBuild {
    /// The configuration naming these databases directly, whose index the
    /// build shares; empty when none does.
    std::string configuration;

    /// The URIs of the databases the links resolve to, in priority order.
    std::vector<std::string> databases;
};

struct ListConfigurationsResult {
    /// The distinct `configuration` tags of the rules, in declaration
    /// order; empty when the rules declare none.
    std::vector<std::string> configurations;

    /// The configuration this server process runs.
    std::string active;

    /// The build this server process runs, when the active configuration
    /// follows links.
    std::optional<LinkedBuild> build;

    /// The persisted selection, applied at the next server start; empty
    /// when none was made.
    std::string selected;

    /// The configuration active when nothing selects one.
    std::string default_configuration;
};

/// clice/switchConfiguration: persist `name` as the selected
/// configuration. The running server keeps its configuration; it asks for
/// the restart that applies the choice (clice/restartServer). `uri` names
/// the project as in ListConfigurationsParams.
struct SwitchConfigurationParams {
    std::string name;
    std::optional<std::string> uri;
};

struct SwitchConfigurationResult {
    bool success = false;

    /// Why the selection was refused, for the user; absent on success.
    std::optional<std::string> reason;
};

/// clice/counterparts: the files `uri` pairs with — a header's sources, a
/// source's headers, a module's interface and implementation units — best
/// first.
struct CounterpartsParams {
    std::string uri;
};

struct Counterpart {
    std::string uri;

    /// What pairs it with the file, for the user to read ("same name",
    /// "defines 12 of 15 declarations").
    std::vector<std::string> reasons;
};

struct CounterpartsResult {
    std::vector<Counterpart> candidates;

    /// The candidate to go to without asking; none when the user should
    /// pick, and when there is no candidate.
    std::optional<std::string> preferred;
};

/// What a client declares under its capabilities' `experimental` key.
struct ExperimentalCapabilities {
    /// The client restarts the server when asked (RestartServerParams):
    /// it owns the server process.
    bool restart_server = false;
};

/// clice/restartServer, sent to a client that declared it handles it: what
/// the server was started on changed, and only a new server applies it.
/// The client restarts the server as its restart command does.
struct RestartServerParams {
    /// What changed, for the user.
    std::string reason;
};

}  // namespace clice::ext

namespace kotatsu::ipc::protocol {

template <>
struct RequestTraits<clice::ext::RestartServerParams> {
    using Result = null;
    constexpr inline static std::string_view method = "clice/restartServer";
};

}  // namespace kotatsu::ipc::protocol
