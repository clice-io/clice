/// Typed definitions of clice's custom LSP extensions — the single source
/// shared by the integration tests and the VSCode extension. Wire shapes
/// mirror src/server/extension.h (camelCase on the wire).

import { NotificationType, RequestType, RequestType0 } from "vscode-languageserver-protocol";

/// A selectable compilation context of a file.
export interface ContextItem {
    label: string;
    description: string;

    /// Host source file (header contexts) or the file itself (source
    /// compile configurations).
    uri: string;

    /// For header contexts: which place the host's compile enters the
    /// header this context represents (0-based, in the order it does).
    /// Present only when it enters the header more than once.
    occurrence?: number;

    /// For source compile configurations: canonical hash identifying the
    /// CDB entry. Pass it back in switchContext to select this entry.
    commandHash?: string;
}

export interface QueryContextParams {
    uri: string;
    offset?: number;
}

export interface QueryContextResult {
    contexts: ContextItem[];
    total: number;

    /// Workspace state generation these results were computed against.
    /// Pass it back in switchContext to detect stale listings.
    epoch: number;
}

export const QueryContextRequest = new RequestType<QueryContextParams, QueryContextResult, void>(
    "clice/queryContext",
);

export interface CurrentContextParams {
    uri: string;
}

export interface CurrentContextResult {
    /// The context the file compiles under, as queryContext lists it:
    /// the user's choice, else the one picked automatically; null when
    /// the file borrows no context and has no entry of its own.
    context: ContextItem | null;

    /// Whether no choice of the user's is in force.
    automatic: boolean;

    /// The listing generation it answers under (QueryContextResult.epoch):
    /// a listing of another epoch is out of date.
    epoch: number;
}

export const CurrentContextRequest = new RequestType<
    CurrentContextParams,
    CurrentContextResult,
    void
>("clice/currentContext");

export interface SwitchContextParams {
    uri: string;
    contextUri: string;

    /// Include occurrence to pin (header contexts, 0-based).
    occurrence?: number;

    /// Canonical CDB entry hash to pin (source files with multiple
    /// compile commands).
    commandHash?: string;

    /// Epoch of the queryContext result this choice came from. When set
    /// and the workspace has changed since, the switch is rejected with
    /// stale = true and the client should re-query.
    epoch?: number;
}

export interface SwitchContextResult {
    success: boolean;

    /// The request referenced an outdated queryContext listing.
    stale: boolean;
}

export const SwitchContextRequest = new RequestType<SwitchContextParams, SwitchContextResult, void>(
    "clice/switchContext",
);

/// clice/resetContext: drop the user's choice, back to the automatic one.
export interface ResetContextParams {
    uri: string;
}

export const ResetContextRequest = new RequestType<ResetContextParams, SwitchContextResult, void>(
    "clice/resetContext",
);

/// clice/listConfigurations: the build configuration menu (the distinct
/// `configuration` tags of the rules) and the names the selection layers
/// hold.
export interface ListConfigurationsResult {
    /// Declared tags in declaration order; empty when the rules declare none.
    configurations: string[];

    /// The configuration this server process runs.
    active: string;

    /// The persisted selection, applied at the next server start; empty
    /// when none was made.
    selected: string;

    /// The configuration active when nothing selects one.
    defaultConfiguration: string;
}

/// clice/listConfigurations: the menu of the project serving `uri`;
/// without one, of the first project over a folder.
export interface ListConfigurationsParams {
    uri?: string;
}

export const ListConfigurationsRequest = new RequestType<
    ListConfigurationsParams,
    ListConfigurationsResult,
    void
>("clice/listConfigurations");

/// clice/switchConfiguration: persist `name` as the selected configuration.
/// The running server keeps its configuration; the choice takes effect when
/// the client restarts it.
export interface SwitchConfigurationParams {
    name: string;
    /// The project, as in ListConfigurationsParams.
    uri?: string;
}

export interface SwitchConfigurationResult {
    success: boolean;
}

export const SwitchConfigurationRequest = new RequestType<
    SwitchConfigurationParams,
    SwitchConfigurationResult,
    void
>("clice/switchConfiguration");

/// clice/internal/poll — TEST-ONLY, not a stable API. Synchronously runs
/// one file-tracker tick (stat → diff → events → dispatch → effects) and
/// responds only once the effects are applied, so integration tests can
/// disable the polling loops and get "change disk → poll → assert"
/// determinism with zero sleeps. Absent from capabilities and user docs.
export interface PollParams {
    /// Which loop to tick: "cdb" or "workspace".
    loop: "cdb" | "workspace";
    /// CDB loop only; defaults to true. A forced tick reloads unconditionally,
    /// skipping the (size, mtime) stamp gate and the two-tick settling
    /// debounce, so one request applies a change deterministically. `false`
    /// runs the production tick: a rewrite is noticed only through its stamp,
    /// and a changed stamp must hold for two consecutive ticks to reload.
    force?: boolean;
}

export interface PollResult {
    /// Number of file events the tick produced and dispatched.
    events: number;
}

export const PollRequest = new RequestType<PollParams, PollResult, void>("clice/internal/poll");

/// Test hook (clice/internal/logFlood): emit `count` info-level log lines
/// of roughly `size` bytes each, tagged stderr-flood with a running index.
/// Gives backpressure tests a deterministic volume source.
export interface LogFloodParams {
    count: number;
    size: number;
}

export interface LogFloodResult {
    emitted: number;
}

export const LogFloodRequest = new RequestType<LogFloodParams, LogFloodResult, void>(
    "clice/internal/logFlood",
);

/// clice/internal/stats — TEST-ONLY, not a stable API. Ownership gauges
/// for memory-lifecycle regression tests: each leak class is pinned by a
/// deterministic counter instead of brittle RSS assertions; and the counts
/// of freshness checks, which pin what a request looks at.
export interface StatsResult {
    pchLoadedStates: number;
    pchStateBytes: number;
    indexInmemoryShards: number;
    indexShardContentBytes: number;
    lastSaveShards: number;
    pendingTmpFiles: number;
    pchCacheEntries: number;
    headerContexts: number;
    synthesizedContexts: number;
    sessions: number;
    /// Freshness checks of files answered by a look at the disk, and from a
    /// look not yet due.
    checksLooked: number;
    checksTrusted: number;
    /// Preprocessor passes that looked for a unit's imports.
    importScans: number;
    /// The builds each file went through, while test hooks are on.
    builds: FileBuilds[];
}

export interface FileBuilds {
    uri: string;
    compile: number;
    pch: number;
    pcm: number;
    index: number;
}

export const StatsRequest = new RequestType0<StatsResult, void>("clice/internal/stats");

/// clice/internal/sync — TEST-ONLY, present while project.test_hooks is on.
/// Answers once the server has no work left: no other request of the editor
/// unanswered, no compile, PCH or PCM build and no index round in flight or
/// queued, a round's save included. The metadata save that follows a build
/// is not waited for; a test needing what is on disk stops the server.
export interface SyncParams {
    /// How long to wait before answering with the work still pending; four
    /// minutes by default.
    deadlineMs?: number;
    /// Tick the workspace loop first, so changes on disk are part of the work
    /// waited for.
    poll?: boolean;
}

export interface SyncResult {
    /// URIs of the files whose latest index attempt failed for good.
    failed: string[];
    /// Index state remains that no save committed.
    unsaved: boolean;
    /// What still ran at the deadline, one line each; empty once settled.
    pending: string[];
}

export const SyncRequest = new RequestType<SyncParams, SyncResult, void>("clice/internal/sync");

/// The builds a hold can park the reply of.
export type BuildKind = "compile" | "pch" | "pcm" | "index";

/// clice/internal/hold — TEST-ONLY, present while project.test_hooks is on.
/// Parks the next reply of a build of `kind` for the file between its arrival
/// and its delivery: to the server the build is still in flight.
/// clice/internal/held announces the parked reply; clice/internal/release
/// lets it go on, or drops a hold no reply reached.
export interface HoldParams {
    kind: BuildKind;
    uri: string;
}

export interface HoldResult {
    id: number;
}

export const HoldRequest = new RequestType<HoldParams, HoldResult, void>("clice/internal/hold");

export interface ReleaseParams {
    id: number;
}

export type ReleaseResult = Record<string, never>;

export const ReleaseRequest = new RequestType<ReleaseParams, ReleaseResult, void>(
    "clice/internal/release",
);

export interface HeldParams {
    id: number;
}

export const HeldNotification = new NotificationType<HeldParams>("clice/internal/held");

/// The keys of a wire type, for pinning a live reply's shape against the
/// hand-written C++ struct: the listing is checked complete at compile
/// time (a key the type gains must be added here), and a test compares it
/// with the reply's `Object.keys`.
export function wireKeys<T>() {
    return <const K extends readonly (keyof T)[]>(
        keys: Exclude<keyof T, K[number]> extends never ? K : never,
    ): readonly (keyof T)[] => keys;
}
