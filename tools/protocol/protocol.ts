/// Typed definitions of clice's custom LSP extensions — the single source
/// shared by the integration tests and the VSCode extension. Wire shapes
/// mirror src/server/protocol/extension.h (camelCase on the wire).

import { RequestType, RequestType0 } from "vscode-languageserver-protocol";

/// A selectable compilation context of a file.
export interface ContextItem {
    label: string;
    description: string;

    /// Host source file (header contexts) or the file itself (source
    /// compile configurations).
    uri: string;

    /// For header contexts: which include of the header in its direct
    /// includer this context represents (0-based, in directive order).
    /// Present only when the header is included more than once.
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
    context: ContextItem | null;
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
    stale?: boolean;
}

export const SwitchContextRequest = new RequestType<SwitchContextParams, SwitchContextResult, void>(
    "clice/switchContext",
);

/// clice/internal/poll — TEST-ONLY, not a stable API. Synchronously runs
/// one file-tracker tick (stat → diff → events → dispatch → effects) and
/// responds only once the effects are applied, so integration tests can
/// disable the polling loops and get "change disk → poll → assert"
/// determinism with zero sleeps. Absent from capabilities and user docs.
export interface PollParams {
    /// Which loop to tick: "cdb" or "workspace".
    loop: "cdb" | "workspace";
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
/// deterministic counter instead of brittle RSS assertions.
export interface StatsResult {
    pchLoadedStates: number;
    pchStateBytes: number;
    indexInmemoryShards: number;
    indexShardContentBytes: number;
    lastSaveShards: number;
    pendingTmpFiles: number;
    [key: string]: number;
}

export const StatsRequest = new RequestType0<StatsResult, void>("clice/internal/stats");
