/** CliceClient — LSP client for integration testing, on vscode-jsonrpc. */

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

// Sanitizer/crash fingerprints scanned in server stderr. Detection happens
// incrementally in the pump: a mid-session report (e.g. relayed from a
// crashed worker) must survive the retention cap's eviction.
export const SANITIZER_MARKERS = [
  "AddressSanitizer",
  "LeakSanitizer",
  "MemorySanitizer",
  "ThreadSanitizer",
  "UndefinedBehaviorSanitizer",
  "==ERROR:",
  "runtime error:",
] as const;

const SANITIZER_MARKER_BUFFERS = SANITIZER_MARKERS.map((m) => Buffer.from(m));

export function withTimeout<T>(
  promise: Promise<T>,
  ms: number,
  what: string,
): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`timed out after ${ms}ms: ${what}`)),
      ms,
    );
    promise.then(
      (v) => {
        clearTimeout(timer);
        resolve(v);
      },
      (e) => {
        clearTimeout(timer);
        reject(e);
      },
    );
  });
}

export interface StartOptions {
  /** The server treats stderr as best-effort, but a client that never
   * reads it forfeits the full mirror (lines are dropped once the pipe
   * fills). Drained continuously by default; backpressure tests opt out
   * to play the hostile client. */
  drainStderr?: boolean;
  args?: string[];
}

export interface InitializeOptions {
  initializationOptions?: Record<string, unknown>;
}

export class CliceClient {
  child: ChildProcessWithoutNullStreams;
  connection: proto.ProtocolConnection;

  diagnostics = new Map<string, proto.Diagnostic[]>();
  logMessages: proto.LogMessageParams[] = [];
  progressTokens: string[] = [];
  progressEvents: Array<{ token: string; value: unknown }> = [];
  initResult: proto.InitializeResult | null = null;
  workspace: string | null = null;

  stderrChunks: Buffer[] = [];
  stderrRetained = 0;
  stderrDrainedFromStart = true;
  stderrMarkerHit: Buffer | null = null;
  stderrScanCarry: Buffer = Buffer.alloc(0);
  private stderrPumping = false;
  /** Resolves when the stderr stream reaches EOF (server closed/exited). */
  stderrEof: Promise<void>;

  exited: Promise<number | null>;

  private diagnosticsWaiters = new Map<string, Array<() => void>>();

  // Retention cap for drained stderr: long stress runs mirror the whole
  // server log, and the teardown scans only need the tail (sanitizer
  // reports and crash text arrive at exit).
  static STDERR_RETAIN_BYTES = 8 * 1024 * 1024;

  private constructor(child: ChildProcessWithoutNullStreams) {
    this.child = child;
    this.connection = createProtocolConnection(
      new StreamMessageReader(child.stdout),
      new StreamMessageWriter(child.stdin),
    );
    this.exited = new Promise((resolve) => {
      child.on("exit", (code) => resolve(code));
    });
    this.stderrEof = new Promise((resolve) => {
      child.stderr.on("close", () => resolve());
    });

    this.connection.onNotification(
      proto.PublishDiagnosticsNotification.type,
      (params) => {
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
            waiters.forEach((resolve) => resolve());
          }
        }
      },
    );
    this.connection.onNotification(proto.LogMessageNotification.type, (p) => {
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
    raw.onRequest((method) => {
      console.warn(`[client] unhandled server request ${method} -> null`);
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
    const client = new CliceClient(child);
    client.stderrDrainedFromStart = options.drainStderr !== false;
    if (client.stderrDrainedFromStart) {
      client.spawnStderrPump();
    }
    return client;
  }

  /** Start the continuous stderr drain if none is running. Backpressure
   * tests start it late: an unread pipe fills and blocks the server, and
   * process exit cannot be observed until reading resumes. */
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
        this.stderrRetained -= this.stderrChunks.shift()!.length;
      }
    });
  }

  /** Latch the earliest sanitizer fingerprint and keep appending context
   * from later reads; the carry covers markers split across read
   * boundaries. */
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
    const hits = SANITIZER_MARKER_BUFFERS.map((m) => window.indexOf(m)).filter(
      (at) => at >= 0,
    );
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

  normalizeUri(uri: string): string {
    try {
      return decodeURIComponent(uri);
    } catch {
      return uri;
    }
  }

  pathToUri(filepath: string): string {
    return this.normalizeUri(URI.file(filepath).toString());
  }

  async initialize(
    workspace: string,
    options: InitializeOptions = {},
  ): Promise<proto.InitializeResult> {
    const initializationOptions = { ...(options.initializationOptions ?? {}) };
    const project = {
      ...((initializationOptions["project"] as Record<string, unknown>) ?? {}),
    };
    // Force cache_dir into the workspace so .clice/ cleanup prevents
    // stale PCH.
    project["cache_dir"] = path.join(workspace, ".clice");
    // One worker of each kind is enough for tests and halves the
    // per-test process-spawn cost. Tests needing more pass their own
    // counts via initializationOptions.
    project["stateless_worker_count"] ??= 1;
    project["stateful_worker_count"] ??= 1;
    initializationOptions["project"] = project;
    // Disable the stat-polling loops: tests drive ticks deterministically
    // through the clice/internal/poll hook instead.
    const tracker = {
      ...((initializationOptions["tracker"] as Record<string, unknown>) ?? {}),
    };
    tracker["cdb_poll_seconds"] ??= 0;
    tracker["workspace_poll_seconds"] ??= 0;
    initializationOptions["tracker"] = tracker;

    const wsUri = URI.file(workspace).toString();
    const params: proto.InitializeParams = {
      processId: process.pid,
      capabilities: {},
      rootUri: wsUri,
      workspaceFolders: [{ uri: wsUri, name: "test" }],
      initializationOptions,
    };
    const result = await this.connection.sendRequest(
      proto.InitializeRequest.type,
      params,
    );
    await this.connection.sendNotification(
      proto.InitializedNotification.type,
      {},
    );
    this.initResult = result;
    this.workspace = workspace;
    return result;
  }

  /** Open a text document. Returns [normalizedUri, content].
   *
   * `text` overrides the on-disk content (an editor buffer may differ
   * from disk); annotated snapshot fixtures open their stripped text
   * this way. */
  open(
    filepath: string,
    version = 0,
    options: { text?: string } = {},
  ): [string, string] {
    const content = options.text ?? fs.readFileSync(filepath, "utf8");
    const wireUri = URI.file(filepath).toString();
    void this.connection.sendNotification(
      proto.DidOpenTextDocumentNotification.type,
      {
        textDocument: {
          uri: wireUri,
          languageId: "cpp",
          version,
          text: content,
        },
      },
    );
    return [this.normalizeUri(wireUri), content];
  }

  close(uri: string): void {
    void this.connection.sendNotification(
      proto.DidCloseTextDocumentNotification.type,
      { textDocument: { uri } },
    );
  }

  /** Full-document didChange. */
  change(uri: string, version: number, text: string): void {
    void this.connection.sendNotification(
      proto.DidChangeTextDocumentNotification.type,
      {
        textDocument: { uri, version },
        contentChanges: [{ text }],
      },
    );
  }

  save(uri: string, text?: string): void {
    void this.connection.sendNotification(
      proto.DidSaveTextDocumentNotification.type,
      { textDocument: { uri }, text },
    );
  }

  /** Arm a waiter that resolves on the NEXT publishDiagnostics for uri. */
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

  /** Open a file and trigger compilation via hover. Waits for diagnostics. */
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

  private textDocumentPosition(uri: string, line: number, character: number) {
    return { textDocument: { uri }, position: { line, character } };
  }

  hoverAt(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.HoverRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  definitionAt(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.DefinitionRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  declarationAt(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.DeclarationRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  implementationAt(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.ImplementationRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  typeDefinitionAt(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
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
    return this.connection.sendRequest(proto.ReferencesRequest.type, {
      ...this.textDocumentPosition(uri, line, character),
      context: { includeDeclaration: options.includeDeclaration ?? true },
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
    return this.connection.sendRequest(proto.CompletionRequest.type, {
      ...this.textDocumentPosition(uri, line, character),
      context,
    });
  }

  signatureHelpAt(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.SignatureHelpRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  documentSymbols(uri: string) {
    return this.connection.sendRequest(proto.DocumentSymbolRequest.type, {
      textDocument: { uri },
    });
  }

  foldingRanges(uri: string) {
    return this.connection.sendRequest(proto.FoldingRangeRequest.type, {
      textDocument: { uri },
    });
  }

  semanticTokensFull(uri: string) {
    return this.connection.sendRequest(proto.SemanticTokensRequest.type, {
      textDocument: { uri },
    });
  }

  inlayHints(uri: string, range: proto.Range) {
    return this.connection.sendRequest(proto.InlayHintRequest.type, {
      textDocument: { uri },
      range,
    });
  }

  codeActions(
    uri: string,
    range: proto.Range,
    diagnostics: proto.Diagnostic[] = [],
  ) {
    return this.connection.sendRequest(proto.CodeActionRequest.type, {
      textDocument: { uri },
      range,
      context: { diagnostics },
    });
  }

  documentLinks(uri: string) {
    return this.connection.sendRequest(proto.DocumentLinkRequest.type, {
      textDocument: { uri },
    });
  }

  formatDocument(uri: string) {
    return this.connection.sendRequest(proto.DocumentFormattingRequest.type, {
      textDocument: { uri },
      options: { tabSize: 4, insertSpaces: true },
    });
  }

  formatRange(uri: string, range: proto.Range) {
    return this.connection.sendRequest(
      proto.DocumentRangeFormattingRequest.type,
      {
        textDocument: { uri },
        range,
        options: { tabSize: 4, insertSpaces: true },
      },
    );
  }

  workspaceSymbols(query: string) {
    return this.connection.sendRequest(proto.WorkspaceSymbolRequest.type, {
      query,
    });
  }

  prepareCallHierarchy(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.CallHierarchyPrepareRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  callHierarchyIncoming(item: proto.CallHierarchyItem) {
    return this.connection.sendRequest(
      proto.CallHierarchyIncomingCallsRequest.type,
      { item },
    );
  }

  callHierarchyOutgoing(item: proto.CallHierarchyItem) {
    return this.connection.sendRequest(
      proto.CallHierarchyOutgoingCallsRequest.type,
      { item },
    );
  }

  prepareTypeHierarchy(uri: string, line: number, character: number) {
    return this.connection.sendRequest(
      proto.TypeHierarchyPrepareRequest.type,
      this.textDocumentPosition(uri, line, character),
    );
  }

  typeHierarchySupertypes(item: proto.TypeHierarchyItem) {
    return this.connection.sendRequest(
      proto.TypeHierarchySupertypesRequest.type,
      { item },
    );
  }

  typeHierarchySubtypes(item: proto.TypeHierarchyItem) {
    return this.connection.sendRequest(
      proto.TypeHierarchySubtypesRequest.type,
      { item },
    );
  }

  queryContext(uri: string, options: { offset?: number } = {}) {
    const params: Record<string, unknown> = { uri };
    if (options.offset !== undefined) {
      params["offset"] = options.offset;
    }
    return this.connection.sendRequest("clice/queryContext", params);
  }

  currentContext(uri: string) {
    return this.connection.sendRequest("clice/currentContext", { uri });
  }

  switchContext(
    uri: string,
    contextUri: string,
    options: {
      occurrence?: number;
      commandHash?: string;
      epoch?: number;
    } = {},
  ) {
    const params: Record<string, unknown> = { uri, contextUri };
    if (options.occurrence !== undefined) {
      params["occurrence"] = options.occurrence;
    }
    if (options.commandHash !== undefined) {
      params["commandHash"] = options.commandHash;
    }
    if (options.epoch !== undefined) {
      params["epoch"] = options.epoch;
    }
    return this.connection.sendRequest("clice/switchContext", params);
  }

  /** clice/internal/poll (test hook): run one tracker tick and apply its
   * effects synchronously. `loop` is "cdb" or "workspace". */
  poll(loop: "cdb" | "workspace") {
    return this.connection.sendRequest("clice/internal/poll", { loop });
  }

  /** clice/internal/stats (test hook): ownership gauges for
   * memory-lifecycle assertions. */
  stats() {
    return this.connection.sendRequest("clice/internal/stats", {});
  }

  /** Force-kill the server process, simulating a crash. */
  killServer(): void {
    this.child.kill("SIGKILL");
  }

  /** Tear down client-side IO without contacting the server. */
  dispose(): void {
    try {
      this.connection.dispose();
    } catch {
      // Already torn down.
    }
  }
}
