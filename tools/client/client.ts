/// CliceClient — LSP client for integration testing, on
/// vscode-languageserver-protocol.
///
/// The client owns its whole lifecycle: spawn (stdio or socket), typed
/// requests including clice's custom protocol, diagnostics tracking,
/// stderr pump with sanitizer-marker latching, graceful shutdown with the
/// clean-exit gate, and the anomaly gate over the bound workspace's logs.

import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import * as fs from "node:fs";
import * as net from "node:net";
import * as path from "node:path";
import type { Readable, Writable } from "node:stream";
import * as proto from "vscode-languageserver-protocol";
import {
    createProtocolConnection,
    StreamMessageReader,
    StreamMessageWriter,
} from "vscode-languageserver-protocol/node";
import { URI } from "vscode-uri";
import {
    CurrentContextRequest,
    LogFloodRequest,
    PollRequest,
    QueryContextRequest,
    StatsRequest,
    SwitchContextRequest,
    type CurrentContextResult,
    type LogFloodResult,
    type PollResult,
    type QueryContextResult,
    type StatsResult,
    type SwitchContextResult,
} from "../protocol/protocol.ts";
import {
    anomalyGateFailure,
    anomaliesInMessages,
    processGateFailures,
    SANITIZER_MARKERS,
    serverStderrExcerpt,
} from "../process_gate.ts";
import { canonicalUri, Workspace } from "./workspace.ts";

export {
    anomaliesInLogFiles,
    crashTracesInLogFiles,
    logFiles,
    SANITIZER_MARKERS,
} from "../process_gate.ts";

// Standard timing constants — use these instead of hardcoded sleep values.
export const MTIME_GRANULARITY = 1_100; // Filesystem mtime precision + margin
export const SETTLE_TIME = 500; // Server stabilization after an operation
export const IDLE_TIMEOUT = 5_000; // Idle soak time in lifecycle tests

export function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export interface WaitUntilOptions {
    timeout: number;
    interval: number;
    description: string;
}

function formatWaitState(state: unknown): string {
    try {
        return JSON.stringify({ value: state });
    } catch {
        return String(state);
    }
}

export async function waitUntil<T>(
    predicate: () => T | Promise<T>,
    { timeout, interval, description }: WaitUntilOptions,
): Promise<T> {
    const deadline = Date.now() + timeout;
    let lastState: T;
    for (;;) {
        lastState = await predicate();
        if (lastState) {
            return lastState;
        }
        const remaining = deadline - Date.now();
        if (remaining <= 0) {
            break;
        }
        await sleep(Math.min(interval, remaining));
    }
    throw new Error(
        `Timed out after ${timeout}ms waiting for ${description}; ` +
            `last state: ${formatWaitState(lastState)}`,
    );
}

/// Normalize a definition/references response to a list of Locations.
export function locationsOf<T>(result: T | T[] | null | undefined): T[] {
    if (result === null || result === undefined) {
        return [];
    }
    return Array.isArray(result) ? result : [result];
}

/// Narrow a definition/declaration response (which may be typed as
/// LocationLink[]) to plain Locations; the server always returns Locations.
export function asLocations(result: unknown): proto.Location[] {
    return locationsOf(result as proto.Location | proto.Location[] | null);
}

const SANITIZER_MARKER_BUFFERS = SANITIZER_MARKERS.map((m) => Buffer.from(m));

export function withTimeout<T>(promise: Promise<T>, ms: number, what: string): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => {
            reject(new Error(`timed out after ${ms}ms: ${what}`));
        }, ms);
        promise.then(
            (v) => {
                clearTimeout(timer);
                resolve(v);
            },
            (e: unknown) => {
                clearTimeout(timer);
                reject(e instanceof Error ? e : new Error(String(e)));
            },
        );
    });
}

let nextPortOffset = 0;

function tryBind(port: number): Promise<boolean> {
    return new Promise((resolve) => {
        const server = net.createServer();
        server.once("error", () => {
            resolve(false);
        });
        server.listen(port, "127.0.0.1", () => {
            server.close(() => {
                resolve(true);
            });
        });
    });
}

/// Pick a port from a per-worker range.
///
/// bind(0) draws from the kernel's shared pool: two concurrent workers can
/// grab the same port in the close-then-rebind gap. Disjoint per-worker
/// ranges (below the ephemeral range) remove that race; the advancing
/// offset avoids immediately reusing a just-released port.
export async function findFreePort(): Promise<number> {
    const index = Number(process.env["VITEST_POOL_ID"] ?? "0") || 0;
    const base = 21000 + index * 100;
    for (let i = 0; i < 100; i++) {
        const port = base + nextPortOffset;
        nextPortOffset = (nextPortOffset + 1) % 100;
        if (await tryBind(port)) {
            return port;
        }
    }
    throw new Error(`no free port in range ${base}-${base + 99}`);
}

export interface StartOptions {
    /// The server treats stderr as best-effort, but a client that never
    /// reads it forfeits the full mirror (lines are dropped once the pipe
    /// fills). Drained continuously by default; backpressure tests opt out
    /// to play the hostile client.
    drainStderr?: boolean | undefined;
    args?: string[] | undefined;
}

export interface InitializeOptions {
    initializationOptions?: Record<string, unknown> | undefined;
    /// Client capabilities to advertise; empty by default so servers see
    /// the most conservative client unless a test opts in.
    capabilities?: proto.ClientCapabilities | undefined;
}

interface Transport {
    reader: Readable;
    writer: Writable;
}

export class CliceClient {
    child: ChildProcessWithoutNullStreams;
    protected connection: proto.ProtocolConnection;
    /// Non-null only in socket mode: the LSP transport rides this socket
    /// instead of the child's stdio, and must be torn down with the client.
    private socket: net.Socket | null = null;

    diagnostics = new Map<string, proto.Diagnostic[]>();
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

    // Retention cap for drained stderr: long stress runs mirror the whole
    // server log, and the teardown scans only need the tail (sanitizer
    // reports and crash text arrive at exit).
    static STDERR_RETAIN_BYTES = 8 * 1024 * 1024;

    private constructor(child: ChildProcessWithoutNullStreams, transport: Transport) {
        this.child = child;
        this.connection = createProtocolConnection(
            new StreamMessageReader(transport.reader),
            new StreamMessageWriter(transport.writer),
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
            const rawUri = params.uri;
            const normalized = this.normalizeUri(rawUri);
            const diags = [...params.diagnostics];
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
        });
        const client = new CliceClient(child, { reader: child.stdout, writer: child.stdin });
        client.stderrDrainedFromStart = options.drainStderr !== false;
        if (client.stderrDrainedFromStart) {
            client.spawnStderrPump();
        }
        return client;
    }

    /// Spawn a server in `--mode socket` and connect the LSP transport over
    /// TCP instead of stdio. The listener isn't ready the instant the
    /// process starts, so poll-connect until it accepts (or the process
    /// dies / times out). stderr stays on the child's pipe and is drained.
    static async startSocket(
        executable: string,
        port: number,
        options: { host?: string | undefined; args?: string[] | undefined } = {},
    ): Promise<CliceClient> {
        const host = options.host ?? "127.0.0.1";
        const child = spawn(
            executable,
            options.args ?? ["serve", "--mode", "socket", "--port", String(port)],
            { stdio: ["pipe", "pipe", "pipe"] },
        );
        let socket: net.Socket | null = null;
        for (let i = 0; i < 150; i++) {
            if (child.exitCode !== null) {
                child.kill("SIGKILL");
                throw new Error("server exited before accepting connections");
            }
            try {
                socket = await connectSocket(host, port);
                break;
            } catch {
                await sleep(200);
            }
        }
        if (socket === null) {
            child.kill("SIGKILL");
            throw new Error(`server did not listen on port ${port} within 30s`);
        }
        const client = new CliceClient(child, { reader: socket, writer: socket });
        client.socket = socket;
        client.stderrDrainedFromStart = true;
        client.spawnStderrPump();
        return client;
    }

    // === Raw wire access =================================================

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
        const initializationOptions = { ...(options.initializationOptions ?? {}) };
        const project = {
            ...((initializationOptions["project"] ?? {}) as Record<string, unknown>),
        };
        // Force cache_dir into the workspace so .clice/ cleanup prevents
        // stale PCH.
        project["cache_dir"] = ws.path(".clice");
        // One worker of each kind is enough for tests and halves the
        // per-test process-spawn cost. Tests needing more pass their own
        // counts via initializationOptions.
        project["stateless_worker_count"] ??= 1;
        project["stateful_worker_count"] ??= 1;
        initializationOptions["project"] = project;
        // Disable the stat-polling loops: tests drive ticks deterministically
        // through the clice/internal/poll hook instead.
        const tracker = {
            ...((initializationOptions["tracker"] ?? {}) as Record<string, unknown>),
        };
        tracker["cdb_poll_seconds"] ??= 0;
        tracker["workspace_poll_seconds"] ??= 0;
        initializationOptions["tracker"] = tracker;

        // Wire URIs stay percent-encoded (a '#' in the path must travel as
        // %23, not become a fragment); the decoded ws.uri() form is for
        // identity comparisons only.
        const wsUri = URI.file(ws.root).toString();
        const params: proto.InitializeParams = {
            processId: process.pid,
            capabilities: options.capabilities ?? {},
            rootUri: wsUri,
            workspaceFolders: [{ uri: wsUri, name: "test" }],
            initializationOptions,
        };
        this.initResult = await this.sendRequest(proto.InitializeRequest.type, params);
        await this.sendNotification(proto.InitializedNotification.type, {});
        this.workspace = ws;
        return this;
    }

    /// Gracefully shut down: shutdown request, exit notification, then the
    /// clean-exit gate (exit code, stderr EOF, drop report, sanitizer
    /// scan). Marks the client disposed so the session teardown skips it.
    async shutdown(options: { verbose?: boolean } = {}): Promise<void> {
        try {
            await withTimeout(
                this.sendRequest(proto.ShutdownRequest.type),
                10_000,
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
            await this.assertExitedCleanly();
        } finally {
            this.dispose();
        }
    }

    /// The clean-exit gate every session must pass.
    async assertExitedCleanly(timeout = 10_000): Promise<void> {
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
            await withTimeout(this.stderrEof, 2_000, "stderr EOF");
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
        if (this.socket !== null) {
            this.socket.destroy();
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
    /// from disk); annotated snapshot fixtures open their stripped text
    /// this way.
    open(filepath: string, version = 0, options: { text?: string } = {}): [string, string] {
        const target = this.resolvePath(filepath);
        const content = options.text ?? fs.readFileSync(target, "utf8");
        const wireUri = URI.file(target).toString();
        void this.sendNotification(proto.DidOpenTextDocumentNotification.type, {
            textDocument: {
                uri: wireUri,
                languageId: "cpp",
                version,
                text: content,
            },
        });
        return [this.normalizeUri(wireUri), content];
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

    async waitDiagnostics(uri: string, timeout = 30_000): Promise<void> {
        uri = this.normalizeUri(uri);
        if (this.diagnostics.has(uri)) {
            return;
        }
        await withTimeout(this.armDiagnostics(uri), timeout, `diagnostics ${uri}`);
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

    /// Trigger recompilation via hover and wait for fresh diagnostics.
    /// Useful after didChange or on-disk file modifications.
    async waitForRecompile(uri: string, timeout = 60_000): Promise<void> {
        const arrived = this.armDiagnostics(uri);
        await this.hoverAt(uri, 0, 0);
        await withTimeout(arrived, timeout, `diagnostics ${uri}`);
    }

    errors(uri: string): proto.Diagnostic[] {
        return (this.diagnostics.get(uri) ?? []).filter(
            (d) => d.severity === proto.DiagnosticSeverity.Error,
        );
    }

    assertNoErrors(uri: string, msg = ""): void {
        const errors = this.errors(uri);
        if (errors.length > 0) {
            throw new Error(
                msg
                    ? `${msg}: ${JSON.stringify(errors)}`
                    : `Expected no errors, got: ${JSON.stringify(errors)}`,
            );
        }
    }

    assertHasErrors(uri: string, msg = ""): void {
        if (this.errors(uri).length === 0) {
            throw new Error(msg || "Expected at least one error diagnostic");
        }
    }

    assertCleanCompile(uri: string): void {
        const diags = this.diagnostics.get(uri) ?? [];
        if (diags.length > 0) {
            throw new Error(`Expected clean compile, got: ${JSON.stringify(diags)}`);
        }
    }

    // === Anomaly gate ====================================================

    /// Anomaly IDs from window/logMessage notifications (master process).
    anomaliesInLogMessages(): string[] {
        return anomaliesInMessages(this.logMessages.map((message) => message.message));
    }

    /// Assert the session produced zero anomalies (client messages + logs).
    /// Runs in every integration test teardown: anomalies are internal
    /// clice bugs and must never occur on regular paths. `root` overrides
    /// the log directory (defaults to the bound workspace).
    assertNoAnomaly(root?: string | null): void {
        const logsRoot = root !== undefined ? root : (this.workspace?.root ?? null);
        const failure = anomalyGateFailure(this.anomaliesInLogMessages(), logsRoot);
        if (failure !== null) {
            throw new Error(failure);
        }
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

    /// URIs of the references at a position (declaration excluded).
    async referenceUris(uri: string, line: number, character: number): Promise<string[]> {
        const refs = await this.referencesAt(uri, line, character, {
            includeDeclaration: false,
        });
        return (refs ?? []).map((ref) => ref.uri);
    }

    /// Poll references at a position until expectedUri shows up.
    async waitForReference(
        uri: string,
        line: number,
        character: number,
        expectedUri: string,
        timeoutSeconds = 30,
    ): Promise<boolean> {
        for (let i = 0; i < timeoutSeconds; i++) {
            if ((await this.referenceUris(uri, line, character)).includes(expectedUri)) {
                return true;
            }
            await sleep(1_000);
        }
        return false;
    }

    /// Poll workspace/symbol until a specific symbol appears in the index.
    async waitForIndex(uri: string, symbolName = "add", timeoutSeconds = 30): Promise<boolean> {
        await this.hoverAt(uri, 0, 0);
        for (let i = 0; i < timeoutSeconds; i++) {
            const result = await this.workspaceSymbols(symbolName);
            if (result?.some((s) => s.name === symbolName)) {
                return true;
            }
            await sleep(1_000);
        }
        return false;
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

    signatureHelpAt(uri: string, line: number, character: number) {
        return this.sendRequest(
            proto.SignatureHelpRequest.type,
            this.textDocumentPosition(uri, line, character),
        );
    }

    documentSymbols(uri: string) {
        return this.sendRequest(proto.DocumentSymbolRequest.type, {
            textDocument: { uri },
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
    /// token modifier — the wire form of preprocessor-inactive regions.
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

    /// clice/internal/poll (test hook): run one tracker tick and apply its
    /// effects synchronously.
    poll(loop: "cdb" | "workspace"): Promise<PollResult> {
        return this.sendRequest(PollRequest, { loop });
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
}

/// Open a TCP connection, resolving once connected and rejecting on error.
function connectSocket(host: string, port: number): Promise<net.Socket> {
    return new Promise((resolve, reject) => {
        const socket = net.createConnection({ host, port });
        socket.once("connect", () => {
            socket.off("error", reject);
            resolve(socket);
        });
        socket.once("error", reject);
    });
}
