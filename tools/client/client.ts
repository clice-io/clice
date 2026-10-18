/// CliceClient — LSP client for the case framework, the benchmarks and
/// the Zed suite, on vscode-languageserver-protocol.
///
/// The client owns its whole lifecycle: spawn, typed requests including
/// clice's custom protocol, diagnostics tracking, stderr pump with
/// sanitizer-marker latching, and graceful shutdown with the clean-exit
/// gate.

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import {
    createProtocolConnection,
    StreamMessageReader,
    StreamMessageWriter,
} from "vscode-languageserver-protocol/node";
import { URI } from "vscode-uri";
import {
    CounterpartsRequest,
    CurrentContextRequest,
    GateRequest,
    HeldNotification,
    HoldRequest,
    ListConfigurationsRequest,
    LogFloodRequest,
    PollRequest,
    QueryContextRequest,
    ReleaseRequest,
    ResetContextRequest,
    StatsRequest,
    SwitchConfigurationRequest,
    SwitchContextRequest,
    SyncRequest,
    type BuildKind,
    type CounterpartsResult,
    type CurrentContextResult,
    type ListConfigurationsResult,
    type LogFloodResult,
    type PollResult,
    type QueryContextResult,
    type StatsResult,
    type SwitchConfigurationResult,
    type SwitchContextResult,
    type SyncParams,
    type SyncResult,
} from "../protocol/protocol.ts";
import {
    anomaliesInMessages,
    processGateFailures,
    SANITIZER_MARKERS,
    serverEnv,
    serverStderrExcerpt,
} from "../process_gate.ts";
import { canonicalUri, Workspace } from "./workspace.ts";

import { withTimeout } from "../promise.ts";

const SANITIZER_MARKER_BUFFERS = SANITIZER_MARKERS.map((m) => Buffer.from(m));

/// Writes the messages to the server in the order they are sent. Under the
/// test hook CLICE_TEST_JITTER=<ms> (the server's half is test_jitter in
/// src/support/environment.cppm), each first waits a random time up to that
/// many milliseconds, as a slow editor's messages do.
class ServerWriter extends StreamMessageWriter {
    private readonly jitter = Number(process.env["CLICE_TEST_JITTER"] ?? 0);
    private queue: Promise<void> = Promise.resolve();

    override write(message: proto.Message): Promise<void> {
        return this.enqueue(() => super.write(message));
    }

    /// Runs `write` once every write sent before it is done.
    enqueue(write: () => Promise<void> | void): Promise<void> {
        const written = this.queue.then(async () => {
            if (this.jitter > 0) {
                // Time is the feature: the delay is what the hook adds.
                await new Promise((resolve) => setTimeout(resolve, Math.random() * this.jitter));
            }
            await write();
        });
        this.queue = written.catch(() => undefined);
        return written;
    }
}

export interface StartOptions {
    /// The server treats stderr as best-effort, but a client that never
    /// reads it forfeits the full mirror (lines are dropped once the pipe
    /// fills). Drained continuously by default; backpressure tests opt out
    /// to play the hostile client.
    drainStderr?: boolean | undefined;
    args?: string[] | undefined;
    /// Working directory of the server process; the caller's by default.
    cwd?: string | undefined;
    /// Extra environment for the server process, which its workers inherit:
    /// the CLICE_TEST_* hooks.
    env?: Record<string, string> | undefined;
}

export interface InitializeOptions {
    initializationOptions?: Record<string, unknown> | undefined;
    /// The server's cache directory, `.clice` in the workspace by default.
    cacheDir?: string | undefined;
    /// Whether to overlay the test defaults — one worker of each kind,
    /// background polling off, no idle wait before background indexing, and
    /// the test hooks on — onto the initialization options.
    /// A benchmark switches them off to run the server's real defaults,
    /// which stay spelled in one place: the C++ config initializers.
    testDefaults?: boolean | undefined;
    /// Client capabilities to advertise; by default versioned edits,
    /// prepared renames, markdown hover cards and diagnostics' related
    /// information alone, so servers see a conservative client unless a
    /// test opts in.
    capabilities?: proto.ClientCapabilities | undefined;
    /// The workspace folders to announce, workspace-relative; the
    /// workspace root alone when omitted, no root at all (neither folders
    /// nor rootUri) when empty, and the root through rootUri alone — a
    /// client without folder support — when null.
    folders?: string[] | null | undefined;
    /// Runs between the initialize response and the initialized
    /// notification.
    beforeInitialized?: (() => Promise<void>) | undefined;
}

/// The initialization options a client sends for `ws`: the caller's, with
/// the cache pinned into the workspace (so `.clice/` cleanup prevents a
/// stale PCH) unless the caller names another and, unless switched off,
/// the test defaults — one worker of each kind (halves the per-test spawn
/// cost; tests needing more pass their own counts), background polling
/// disabled (tests drive ticks deterministically through the
/// clice/internal/poll hook), background
/// indexing without the idle wait that batches an editor's keystrokes
/// (tests wait through clice/internal/sync, not the clock), and the test
/// hooks on.
export function initializationOptionsFor(
    ws: Workspace,
    options: InitializeOptions,
): Record<string, unknown> {
    const initializationOptions = { ...(options.initializationOptions ?? {}) };
    const project = {
        ...((initializationOptions["project"] ?? {}) as Record<string, unknown>),
    };
    project["cache_dir"] = options.cacheDir ?? ws.path(".clice");
    const tracker = {
        ...((initializationOptions["tracker"] ?? {}) as Record<string, unknown>),
    };
    if (options.testDefaults ?? true) {
        project["stateless_worker_count"] ??= 1;
        project["stateful_worker_count"] ??= 1;
        project["idle_timeout_ms"] ??= 0;
        project["test_hooks"] ??= true;
        tracker["workspace_poll_seconds"] ??= 0;
    }
    initializationOptions["project"] = project;
    initializationOptions["tracker"] = tracker;
    return initializationOptions;
}

export class CliceClient {
    child: ChildProcessWithoutNullStreams;
    protected connection: proto.ProtocolConnection;
    private writer: ServerWriter;

    diagnostics = new Map<string, proto.Diagnostic[]>();
    /// Every publishDiagnostics received, in order.
    publishedDiagnostics: proto.PublishDiagnosticsParams[] = [];
    logMessages: proto.LogMessageParams[] = [];
    progressTokens: string[] = [];
    progressEvents: { token: string; value: unknown }[] = [];
    /// Methods of server→client requests the client answered with null.
    serverRequests: string[] = [];
    initResult: proto.InitializeResult | null = null;
    /// Bound by initialize(); relative paths in open/openAndWait resolve
    /// against it.
    workspace: Workspace | null = null;

    stderrChunks: Buffer[] = [];
    stderrRetained = 0;
    stderrDrainedFromStart = true;
    stderrMarkerHit: Buffer | null = null;
    stderrScanCarry: Buffer = Buffer.alloc(0);
    private stderrPumping = false;
    /// Resolves when the stderr stream reaches EOF (server closed/exited).
    stderrEof: Promise<void>;

    exited: Promise<number | null>;

    /// True once dispose() ran — the session teardown uses it to skip
    /// clients a test already shut down explicitly.
    disposed = false;

    private diagnosticsWaiters = new Map<string, (() => void)[]>();
    private publishes = new Map<string, number>();
    /// Holds whose reply the server parked (clice/internal/held), and the
    /// waiters of the ones not parked yet.
    private parked = new Set<number>();
    private parkWaiters = new Map<number, (() => void)[]>();
    private progressWaiters: { token: string; kind: string; resolve: () => void }[] = [];

    // Retention cap for drained stderr: long stress runs mirror the whole
    // server log, and the teardown scans only need the tail (sanitizer
    // reports and crash text arrive at exit).
    static STDERR_RETAIN_BYTES = 8 * 1024 * 1024;

    private constructor(child: ChildProcessWithoutNullStreams) {
        this.child = child;
        this.writer = new ServerWriter(child.stdin);
        this.connection = createProtocolConnection(
            new StreamMessageReader(child.stdout),
            this.writer,
        );
        this.exited = new Promise((resolve) => {
            child.on("exit", (code) => {
                resolve(code);
            });
        });
        this.stderrEof = new Promise((resolve) => {
            child.stderr.on("close", () => {
                resolve();
            });
        });

        this.onNotification(proto.PublishDiagnosticsNotification.type, (params) => {
            this.publishedDiagnostics.push(params);
            const rawUri = params.uri;
            const normalized = this.normalizeUri(rawUri);
            const diags = [...params.diagnostics];
            this.publishes.set(normalized, (this.publishes.get(normalized) ?? 0) + 1);
            this.diagnostics.set(rawUri, diags);
            if (rawUri !== normalized) {
                this.diagnostics.set(normalized, diags);
            }
            for (const key of [rawUri, normalized]) {
                const waiters = this.diagnosticsWaiters.get(key);
                if (waiters) {
                    this.diagnosticsWaiters.delete(key);
                    waiters.forEach((resolve) => {
                        resolve();
                    });
                }
            }
        });
        this.onNotification(proto.LogMessageNotification.type, (p) => {
            this.logMessages.push(p);
        });
        this.onNotification(HeldNotification, (p) => {
            this.parked.add(p.id);
            for (const resolve of this.parkWaiters.get(p.id) ?? []) {
                resolve();
            }
            this.parkWaiters.delete(p.id);
        });
        this.connection.onRequest(proto.WorkDoneProgressCreateRequest.type, (p) => {
            this.progressTokens.push(String(p.token));
        });
        // ProtocolConnection's type surface omits the star handlers and the
        // unhandled-progress hook its underlying MessageConnection provides;
        // reach through for them.
        const raw = this.connection as unknown as {
            onUnhandledProgress(
                handler: (p: { token: string | number; value: unknown }) => void,
            ): void;
            onRequest(handler: (method: string, params: unknown) => unknown): void;
            onNotification(handler: (method: string, params: unknown) => void): void;
        };
        raw.onUnhandledProgress((p) => {
            this.progressEvents.push({ token: String(p.token), value: p.value });
            const kind = (p.value as { kind?: unknown }).kind;
            const arrived = (waiter: { token: string; kind: string }): boolean =>
                waiter.token === String(p.token) && waiter.kind === kind;
            for (const waiter of this.progressWaiters.filter(arrived)) {
                waiter.resolve();
            }
            this.progressWaiters = this.progressWaiters.filter((waiter) => !arrived(waiter));
        });
        // Requests the test client does not model are answered with null
        // instead of "method not found", mirroring pygls' lenient client.
        // The methods are recorded so tests can assert that a refresh (or
        // any other server-initiated request) was actually sent.
        raw.onRequest((method) => {
            console.warn(`[client] unhandled server request ${method} -> null`);
            this.serverRequests.push(method);
            return null;
        });
        raw.onNotification((method) => {
            console.warn(`[client] ignoring notification for ${method}`);
        });
        this.connection.listen();
    }

    static start(executable: string, options: StartOptions = {}): CliceClient {
        const child = spawn(executable, options.args ?? ["serve"], {
            stdio: ["pipe", "pipe", "pipe"],
            cwd: options.cwd,
            env: { ...serverEnv(), ...options.env },
        });
        const client = new CliceClient(child);
        client.stderrDrainedFromStart = options.drainStderr !== false;
        if (client.stderrDrainedFromStart) {
            client.spawnStderrPump();
        }
        return client;
    }

    // === Raw protocol access =============================================

    sendRequest<P, R, E>(
        type: proto.RequestType<P, R, E>,
        params: P,
        token?: proto.CancellationToken,
    ): Promise<R>;
    sendRequest<R, E>(type: proto.RequestType0<R, E>): Promise<R>;
    sendRequest(
        method: string,
        params?: unknown,
        token?: proto.CancellationToken,
    ): Promise<unknown>;
    sendRequest(
        type: unknown,
        params?: unknown,
        token?: proto.CancellationToken,
    ): Promise<unknown> {
        const conn = this.connection as unknown as {
            sendRequest(type: unknown, ...rest: unknown[]): Promise<unknown>;
        };
        return token === undefined
            ? conn.sendRequest(type, params)
            : conn.sendRequest(type, params, token);
    }

    /// Writes `messages` in one write, which the server reads together, and
    /// resolves with the response to each request among them, by id. The
    /// client's own connection ignores those responses: the ids must be
    /// strings, which it never sends.
    sendTogether(
        messages: (proto.RequestMessage | proto.NotificationMessage)[],
    ): Promise<Map<string, proto.ResponseMessage>> {
        const pending = new Set(
            messages.flatMap((message) => ("id" in message ? [String(message.id)] : [])),
        );
        const responses = new Map<string, proto.ResponseMessage>();
        const { stdout: reader, stdin } = this.child;
        const answered = new Promise<Map<string, proto.ResponseMessage>>((resolve) => {
            if (pending.size === 0) {
                resolve(responses);
                return;
            }
            // Attached mid-stream, the first chunk may begin inside a
            // message: a frame is found by the end of its header, and the
            // length the header names.
            let buffer = Buffer.alloc(0);
            const onData = (chunk: Buffer): void => {
                buffer = Buffer.concat([buffer, chunk]);
                for (;;) {
                    const end = buffer.indexOf("\r\n\r\n");
                    if (end < 0) {
                        break;
                    }
                    const header = buffer.subarray(0, end).toString("latin1");
                    const named = /Content-Length: (\d+)/i.exec(
                        header.slice(header.lastIndexOf("Content-Length:")),
                    );
                    const length = Number(named?.[1] ?? 0);
                    if (buffer.length < end + 4 + length) {
                        break;
                    }
                    const body = buffer.subarray(end + 4, end + 4 + length).toString("utf-8");
                    buffer = buffer.subarray(end + 4 + length);
                    const message = JSON.parse(body) as proto.Message;
                    if (proto.Message.isResponse(message) && pending.delete(String(message.id))) {
                        responses.set(String(message.id), message);
                    }
                }
                if (pending.size === 0) {
                    reader.off("data", onData);
                    resolve(responses);
                }
            };
            reader.on("data", onData);
        });
        void this.writer.enqueue(() => {
            stdin.write(
                Buffer.concat(
                    messages.map((message) => {
                        const body = Buffer.from(JSON.stringify(message), "utf-8");
                        return Buffer.concat([
                            Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii"),
                            body,
                        ]);
                    }),
                ),
            );
        });
        return answered;
    }

    sendNotification<P>(type: proto.NotificationType<P>, params: P): Promise<void>;
    sendNotification(type: proto.NotificationType0): Promise<void>;
    sendNotification(method: string, params?: unknown): Promise<void>;
    sendNotification(type: unknown, params?: unknown): Promise<void> {
        return (
            this.connection as unknown as {
                sendNotification(type: unknown, params?: unknown): Promise<void>;
            }
        ).sendNotification(type, params);
    }

    onNotification<P>(type: proto.NotificationType<P>, handler: (params: P) => void): void;
    onNotification(method: string, handler: (params: unknown) => void): void;
    onNotification(type: unknown, handler: (params: never) => void): void {
        (
            this.connection as unknown as {
                onNotification(type: unknown, handler: unknown): void;
            }
        ).onNotification(type, handler);
    }

    // === Lifecycle =======================================================

    /// Initialize on a workspace; binds it so relative paths resolve
    /// against it from here on. Returns this for chaining; the raw result
    /// stays available as initResult.
    async initialize(
        workspace: Workspace | string,
        options: InitializeOptions = {},
    ): Promise<this> {
        const ws = workspace instanceof Workspace ? workspace : new Workspace(workspace);
        const initializationOptions = initializationOptionsFor(ws, options);

        // Protocol URIs stay percent-encoded (a '#' in the path must travel as
        // %23, not become a fragment); the decoded ws.uri() form is for
        // identity comparisons only.
        const wsUri = URI.file(ws.root).toString();
        const params: proto.InitializeParams = {
            processId: process.pid,
            // Versioned workspace edits and diagnostics' related
            // information, like every editor client: code action replies
            // then carry the buffer version they apply to, and a
            // diagnostic's notes point at their places. Document symbols
            // as a tree in every kind, as every editor takes them.
            capabilities: options.capabilities ?? {
                workspace: { workspaceEdit: { documentChanges: true } },
                textDocument: {
                    hover: { contentFormat: ["markdown", "plaintext"] },
                    rename: { prepareSupport: true },
                    publishDiagnostics: { relatedInformation: true },
                    documentSymbol: {
                        hierarchicalDocumentSymbolSupport: true,
                        symbolKind: { valueSet: Object.values(proto.SymbolKind) },
                    },
                },
            },
            rootUri: options.folders?.length === 0 ? null : wsUri,
            initializationOptions,
        };
        if (options.folders === undefined) {
            params.workspaceFolders = [{ uri: wsUri, name: "test" }];
        } else if (options.folders !== null) {
            params.workspaceFolders = options.folders.map((folder) => ({
                uri: URI.file(ws.path(folder)).toString(),
                name: folder,
            }));
        }
        this.initResult = await this.sendRequest(proto.InitializeRequest.type, params);
        this.workspace = ws;
        await options.beforeInitialized?.();
        await this.sendNotification(proto.InitializedNotification.type, {});
        return this;
    }

    /// Announce workspace folders coming and going
    /// (didChangeWorkspaceFolders), workspace-relative like `folders` at
    /// initialize.
    changeWorkspaceFolders(change: { added?: string[]; removed?: string[] }): Promise<void> {
        const folder = (name: string) => ({
            uri: URI.file(this.resolvePath(name)).toString(),
            name,
        });
        return this.sendNotification(proto.DidChangeWorkspaceFoldersNotification.type, {
            event: {
                added: (change.added ?? []).map(folder),
                removed: (change.removed ?? []).map(folder),
            },
        });
    }

    /// Gracefully shut down: shutdown request, exit notification, then the
    /// clean-exit gate (exit code, stderr EOF, drop report, sanitizer
    /// scan), each wait given `bound` milliseconds. Marks the client
    /// disposed so the session teardown skips it.
    async shutdown(options: { verbose?: boolean; bound?: number } = {}): Promise<void> {
        const bound = options.bound ?? 60_000;
        try {
            await withTimeout(
                this.sendRequest(proto.ShutdownRequest.type),
                bound,
                "shutdown request",
            );
        } catch {
            // The exit-clean gate below reports the real failure.
        }

        try {
            await this.sendNotification(proto.ExitNotification.type);
        } catch {
            // Connection may already be gone; the exit gate decides.
        }

        if (options.verbose && this.logMessages.length > 0) {
            const levels: Record<number, string> = { 1: "ERROR", 2: "WARN", 3: "INFO", 4: "LOG" };
            for (const msg of this.logMessages) {
                console.log(`[logMessage/${levels[msg.type] ?? "?"}] ${msg.message}`);
            }
        }

        try {
            await this.assertExitedCleanly(bound);
        } finally {
            this.dispose();
        }
    }

    /// The clean-exit gate every session must pass.
    async assertExitedCleanly(timeout = 60_000): Promise<void> {
        const failures: string[] = [];

        if (this.child.exitCode === null && this.child.signalCode === null) {
            try {
                await withTimeout(this.exited, timeout, "server exit");
            } catch {
                this.child.kill("SIGKILL");
                await this.exited;
                failures.push(`server did not exit within ${timeout / 1000}s after shutdown`);
            }
        }

        console.log(`[server] exit code: ${this.child.exitCode}`);

        // Collect stderr AFTER the exit wait: exit-time output (sanitizer
        // reports, late crash text) must reach the scan below. The pump owns
        // the stream — wait for it to see EOF instead of racing it with a
        // second reader.
        let stderrComplete = true;
        let stderrFailure: string | undefined;
        try {
            await withTimeout(this.stderrEof, timeout, "stderr EOF");
        } catch (exc) {
            stderrComplete = false;
            stderrFailure = String(exc);
        }
        const stderrText = this.drainedStderr().toString("utf8");

        for (const line of serverStderrExcerpt(stderrText).split("\n")) {
            if (line) {
                console.log(`[server] ${line}`);
            }
        }

        failures.push(
            ...processGateFailures({
                exitCode: this.child.exitCode,
                signalCode: this.child.signalCode,
                stderrText,
                stderrComplete,
                stderrFailure,
                stderrDrainedFromStart: this.stderrDrainedFromStart,
                sanitizerMarkerHit: this.stderrMarkerHit?.toString("utf8"),
            }),
        );

        if (failures.length > 0) {
            const excerpt = serverStderrExcerpt(stderrText);
            if (excerpt) {
                failures.push("server stderr excerpt:\n" + excerpt);
            }
            throw new Error(failures.join("\n"));
        }
    }

    /// Force-kill the server process, simulating a crash.
    killServer(): void {
        this.child.kill("SIGKILL");
    }

    /// Tear down client-side IO without contacting the server.
    dispose(): void {
        this.disposed = true;
        try {
            this.connection.dispose();
        } catch {
            // Already torn down.
        }
    }

    // === stderr pump =====================================================

    /// Start the continuous stderr drain if none is running. Backpressure
    /// tests start it late: an unread pipe fills and blocks the server, and
    /// process exit cannot be observed until reading resumes.
    spawnStderrPump(): void {
        if (this.stderrPumping) {
            return;
        }
        this.stderrPumping = true;
        this.child.stderr.on("data", (data: Buffer) => {
            this.scanForMarkers(data);
            this.stderrChunks.push(data);
            this.stderrRetained += data.length;
            while (
                this.stderrRetained > CliceClient.STDERR_RETAIN_BYTES &&
                this.stderrChunks.length > 1
            ) {
                const evicted = this.stderrChunks.shift();
                if (evicted === undefined) {
                    break;
                }
                this.stderrRetained -= evicted.length;
            }
        });
    }

    /// Detection happens incrementally in the pump: a mid-session report
    /// (e.g. relayed from a crashed worker) must survive the retention
    /// cap's eviction. Latch the earliest sanitizer fingerprint and keep
    /// appending context from later reads; the carry covers markers split
    /// across read boundaries.
    private scanForMarkers(data: Buffer): void {
        if (this.stderrMarkerHit !== null) {
            if (this.stderrMarkerHit.length < 4096) {
                this.stderrMarkerHit = Buffer.concat([
                    this.stderrMarkerHit,
                    data.subarray(0, 4096 - this.stderrMarkerHit.length),
                ]);
            }
            return;
        }
        const window = Buffer.concat([this.stderrScanCarry, data]);
        const hits = SANITIZER_MARKER_BUFFERS.map((m) => window.indexOf(m)).filter((at) => at >= 0);
        if (hits.length > 0) {
            const at = Math.min(...hits);
            this.stderrMarkerHit = window.subarray(at, at + 4096);
            return;
        }
        this.stderrScanCarry = window.subarray(Math.max(0, window.length - 64));
    }

    drainedStderr(): Buffer {
        return Buffer.concat(this.stderrChunks);
    }

    // === Documents =======================================================

    normalizeUri(uri: string): string {
        return canonicalUri(uri);
    }

    /// Workspace-relative paths resolve against the bound workspace;
    /// absolute paths pass through.
    private resolvePath(p: string): string {
        if (path.isAbsolute(p)) {
            return p;
        }
        if (this.workspace === null) {
            throw new Error(`relative path ${p} needs an initialized workspace`);
        }
        return this.workspace.path(p);
    }

    /// Open a text document (path may be workspace-relative). Returns
    /// [normalizedUri, content].
    ///
    /// `text` overrides the on-disk content (an editor buffer may differ
    /// from disk).
    open(filepath: string, version = 0, options: { text?: string } = {}): [string, string] {
        const target = this.resolvePath(filepath);
        const content = options.text ?? fs.readFileSync(target, "utf8");
        const sentUri = URI.file(target).toString();
        void this.sendNotification(proto.DidOpenTextDocumentNotification.type, {
            textDocument: {
                uri: sentUri,
                languageId: "cpp",
                version,
                text: content,
            },
        });
        return [this.normalizeUri(sentUri), content];
    }

    close(uri: string): void {
        void this.sendNotification(proto.DidCloseTextDocumentNotification.type, {
            textDocument: { uri },
        });
    }

    /// Full-document didChange.
    change(uri: string, version: number, text: string): void {
        void this.sendNotification(proto.DidChangeTextDocumentNotification.type, {
            textDocument: { uri, version },
            contentChanges: [{ text }],
        });
    }

    /// Incremental didChange: replace `range` with `text`.
    changeRange(uri: string, version: number, range: proto.Range, text: string): void {
        void this.sendNotification(proto.DidChangeTextDocumentNotification.type, {
            textDocument: { uri, version },
            contentChanges: [{ range, text }],
        });
    }

    save(uri: string, text?: string): void {
        void this.sendNotification(proto.DidSaveTextDocumentNotification.type, {
            textDocument: { uri },
            ...(text === undefined ? {} : { text }),
        });
    }

    // === Diagnostics =====================================================

    /// Arm a waiter that resolves on the NEXT publishDiagnostics for uri.
    armDiagnostics(uri: string): Promise<void> {
        uri = this.normalizeUri(uri);
        return new Promise((resolve) => {
            const waiters = this.diagnosticsWaiters.get(uri) ?? [];
            waiters.push(resolve);
            this.diagnosticsWaiters.set(uri, waiters);
        });
    }

    /// Open a file (path may be workspace-relative) and trigger compilation
    /// via hover. Waits for diagnostics.
    async openAndWait(
        filepath: string,
        timeout = 60_000,
        options: { text?: string } = {},
    ): Promise<[string, string]> {
        const [uri, content] = this.open(filepath, 0, options);
        const arrived = this.armDiagnostics(uri);
        await this.hoverAt(uri, 0, 0);
        await withTimeout(arrived, timeout, `diagnostics ${uri}`);
        return [uri, content];
    }

    /// Pull the document's diagnostics (textDocument/diagnostic); clice
    /// answers every pull with a full report.
    async pullDiagnostics(
        uri: string,
        token?: proto.CancellationToken,
    ): Promise<proto.Diagnostic[]> {
        const report = await this.sendRequest(
            proto.DocumentDiagnosticRequest.type,
            { textDocument: { uri } },
            token,
        );
        if (report.kind !== proto.DocumentDiagnosticReportKind.Full) {
            throw new Error(`Expected a full report, got: ${JSON.stringify(report)}`);
        }
        return report.items;
    }

    /// How many diagnostics publishes the document has received.
    publishCount(uri: string): number {
        return this.publishes.get(this.normalizeUri(uri)) ?? 0;
    }

    // === Anomaly gate ====================================================

    /// Anomaly IDs from window/logMessage notifications (master process).
    anomaliesInLogMessages(): string[] {
        return anomaliesInMessages(this.logMessages.map((message) => message.message));
    }

    /// Guidance texts from window/logMessage notifications.
    guidanceMessages(): string[] {
        return this.logMessages
            .filter((msg) => msg.message.includes("[guidance]"))
            .map((msg) => msg.message);
    }

    // === Standard requests ===============================================

    private textDocumentPosition(uri: string, line: number, character: number) {
        return { textDocument: { uri }, position: { line, character } };
    }

    hoverAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.HoverRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    definitionAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.DefinitionRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    declarationAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.DeclarationRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    implementationAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.ImplementationRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    typeDefinitionAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.TypeDefinitionRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    referencesAt(
        uri: string,
        line: number,
        character: number,
        options: { includeDeclaration?: boolean } = {},
    ) {
        return this.sendRequest(proto.ReferencesRequest.type, {
            ...this.textDocumentPosition(uri, line, character),
            context: { includeDeclaration: options.includeDeclaration ?? true },
        });
    }

    documentHighlightAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.DocumentHighlightRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    prepareRenameAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.PrepareRenameRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    renameAt(uri: string, line: number, character: number, newName: string) {
        return this.sendRequest(proto.RenameRequest.type, {
            ...this.textDocumentPosition(uri, line, character),
            newName,
        });
    }

    completionAt(
        uri: string,
        line: number,
        character: number,
        options: { triggerCharacter?: string } = {},
    ) {
        const context = options.triggerCharacter
            ? {
                  triggerKind: proto.CompletionTriggerKind.TriggerCharacter,
                  triggerCharacter: options.triggerCharacter,
              }
            : undefined;
        return this.sendRequest(proto.CompletionRequest.type, {
            ...this.textDocumentPosition(uri, line, character),
            ...(context === undefined ? {} : { context }),
        });
    }

    /// With `shown`, asked again while the popup shows those signatures,
    /// the `active` one selected, as typing in the call asks.
    signatureHelpAt(
        uri: string,
        line: number,
        character: number,
        shown?: { signatures: readonly string[]; active: number },
    ) {
        const context =
            shown === undefined
                ? undefined
                : {
                      triggerKind: proto.SignatureHelpTriggerKind.ContentChange,
                      isRetrigger: true,
                      activeSignatureHelp: {
                          signatures: shown.signatures.map((label) => ({ label })),
                          activeSignature: shown.active,
                      },
                  };
        return this.sendRequest(proto.SignatureHelpRequest.type, {
            ...this.textDocumentPosition(uri, line, character),
            ...(context === undefined ? {} : { context }),
        });
    }

    documentSymbols(uri: string) {
        return this.sendRequest(proto.DocumentSymbolRequest.type, {
            textDocument: { uri },
        });
    }

    selectionRanges(uri: string, positions: proto.Position[]) {
        return this.sendRequest(proto.SelectionRangeRequest.type, {
            textDocument: { uri },
            positions,
        });
    }

    foldingRanges(uri: string) {
        return this.sendRequest(proto.FoldingRangeRequest.type, {
            textDocument: { uri },
        });
    }

    semanticTokensFull(uri: string) {
        return this.sendRequest(proto.SemanticTokensRequest.type, {
            textDocument: { uri },
        });
    }

    /// Lines carrying at least one token with the `inactive` semantic
    /// token modifier — the protocol form of preprocessor-inactive regions.
    async inactiveLines(uri: string): Promise<number[]> {
        const result = await this.semanticTokensFull(uri);
        const provider = this.initResult?.capabilities.semanticTokensProvider as
            | proto.SemanticTokensOptions
            | undefined;
        const bit = provider?.legend.tokenModifiers.indexOf("inactive") ?? -1;
        if (bit < 0) {
            throw new Error("server legend misses the inactive modifier");
        }
        const data = result?.data ?? [];
        const lines = new Set<number>();
        let line = 0;
        for (let i = 0; i + 4 < data.length; i += 5) {
            line += data[i] ?? 0;
            if (((data[i + 4] ?? 0) & (1 << bit)) !== 0) {
                lines.add(line);
            }
        }
        return [...lines].sort((a, b) => a - b);
    }

    inlayHints(uri: string, range: proto.Range) {
        return this.sendRequest(proto.InlayHintRequest.type, {
            textDocument: { uri },
            range,
        });
    }

    codeActions(uri: string, range: proto.Range, diagnostics: proto.Diagnostic[] = []) {
        return this.sendRequest(proto.CodeActionRequest.type, {
            textDocument: { uri },
            range,
            context: { diagnostics },
        });
    }

    documentLinks(uri: string) {
        return this.sendRequest(proto.DocumentLinkRequest.type, {
            textDocument: { uri },
        });
    }

    formatDocument(uri: string) {
        return this.sendRequest(proto.DocumentFormattingRequest.type, {
            textDocument: { uri },
            options: { tabSize: 4, insertSpaces: true },
        });
    }

    formatRange(uri: string, range: proto.Range) {
        return this.sendRequest(proto.DocumentRangeFormattingRequest.type, {
            textDocument: { uri },
            range,
            options: { tabSize: 4, insertSpaces: true },
        });
    }

    workspaceSymbols(query: string) {
        return this.sendRequest(proto.WorkspaceSymbolRequest.type, { query });
    }

    prepareCallHierarchy(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.CallHierarchyPrepareRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    callHierarchyIncoming(item: proto.CallHierarchyItem) {
        return this.sendRequest(proto.CallHierarchyIncomingCallsRequest.type, { item });
    }

    callHierarchyOutgoing(item: proto.CallHierarchyItem) {
        return this.sendRequest(proto.CallHierarchyOutgoingCallsRequest.type, { item });
    }

    prepareTypeHierarchy(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.TypeHierarchyPrepareRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    typeHierarchySupertypes(item: proto.TypeHierarchyItem) {
        return this.sendRequest(proto.TypeHierarchySupertypesRequest.type, { item });
    }

    typeHierarchySubtypes(item: proto.TypeHierarchyItem) {
        return this.sendRequest(proto.TypeHierarchySubtypesRequest.type, { item });
    }

    // === clice custom protocol ===========================================

    queryContext(uri: string, options: { offset?: number } = {}): Promise<QueryContextResult> {
        return this.sendRequest(QueryContextRequest, {
            uri,
            ...(options.offset === undefined ? {} : { offset: options.offset }),
        });
    }

    currentContext(uri: string): Promise<CurrentContextResult> {
        return this.sendRequest(CurrentContextRequest, { uri });
    }

    resetContext(uri: string): Promise<SwitchContextResult> {
        return this.sendRequest(ResetContextRequest, { uri });
    }

    switchContext(
        uri: string,
        contextUri: string,
        options: { occurrence?: number; commandHash?: string; epoch?: number } = {},
    ): Promise<SwitchContextResult> {
        return this.sendRequest(SwitchContextRequest, {
            uri,
            contextUri,
            ...(options.occurrence === undefined ? {} : { occurrence: options.occurrence }),
            ...(options.commandHash === undefined ? {} : { commandHash: options.commandHash }),
            ...(options.epoch === undefined ? {} : { epoch: options.epoch }),
        });
    }

    /// The build configuration menu of the project serving `uri`, else of
    /// the first project over a folder.
    listConfigurations(uri?: string): Promise<ListConfigurationsResult> {
        return this.sendRequest(ListConfigurationsRequest, uri === undefined ? {} : { uri });
    }

    switchConfiguration(name: string, uri?: string): Promise<SwitchConfigurationResult> {
        return this.sendRequest(
            SwitchConfigurationRequest,
            uri === undefined ? { name } : { name, uri },
        );
    }

    counterparts(uri: string): Promise<CounterpartsResult> {
        return this.sendRequest(CounterpartsRequest, { uri });
    }

    /// clice/internal/poll (test hook): run one tracker tick and apply its
    /// effects synchronously. `force: false` takes the CDB loop through its
    /// real stamp gate and stability debounce (see PollParams).
    poll(loop: "cdb" | "workspace", options: { force?: boolean } = {}): Promise<PollResult> {
        return this.sendRequest(PollRequest, {
            loop,
            ...(options.force === undefined ? {} : { force: options.force }),
        });
    }

    /// clice/internal/stats (test hook): ownership gauges for
    /// memory-lifecycle assertions.
    stats(): Promise<StatsResult> {
        return this.sendRequest(StatsRequest);
    }

    /// clice/internal/logFlood (test hook): deterministic stderr volume.
    logFlood(count: number, size: number): Promise<LogFloodResult> {
        return this.sendRequest(LogFloodRequest, { count, size });
    }

    /// clice/internal/sync (test hook): answers once the server has no work
    /// left, or at the deadline with the work still pending.
    sync(params: SyncParams = {}): Promise<SyncResult> {
        return this.sendRequest(SyncRequest, params);
    }

    /// clice/internal/hold (test hook): park the next reply of a `kind`
    /// build of `uri`; returns the hold's id.
    async hold(kind: BuildKind, uri: string): Promise<number> {
        return (await this.sendRequest(HoldRequest, { kind, uri })).id;
    }

    /// clice/internal/gate (test hook): park the work of the next `request`
    /// for `uri` in its worker, where the work starts; returns the hold's id.
    async gate(request: string, uri: string): Promise<number> {
        return (await this.sendRequest(GateRequest, { request, uri })).id;
    }

    /// Resolves once the server sent `token` a work done progress of `kind`
    /// ("begin", "report", "end"), the ones it sent before included.
    progressed(token: string, kind: string): Promise<void> {
        if (
            this.progressEvents.some(
                (event) =>
                    event.token === token && (event.value as { kind?: unknown }).kind === kind,
            )
        ) {
            return Promise.resolve();
        }
        return new Promise((resolve) => this.progressWaiters.push({ token, kind, resolve }));
    }

    /// Resolves once the hold parked a reply, or the gate a request's work
    /// (clice/internal/held).
    parkedBy(id: number): Promise<void> {
        if (this.parked.has(id)) {
            return Promise.resolve();
        }
        return new Promise((resolve) => {
            this.parkWaiters.set(id, [...(this.parkWaiters.get(id) ?? []), resolve]);
        });
    }

    /// clice/internal/release (test hook): let the parked reply or work go
    /// on, or drop a hold or gate nothing reached.
    async release(id: number): Promise<void> {
        this.parked.delete(id);
        await this.sendRequest(ReleaseRequest, { id });
    }
}
