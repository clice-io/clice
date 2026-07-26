/// Replay recorded LSP traces against clice to detect hangs and crashes.
///
/// Usage: node tools/replay.ts tests/smoke/session.jsonl --clice build/clice
///
/// Node builtins only — no npm dependencies — so it runs standalone with
/// `node tools/replay.ts ...`. LSP framing, response defaults and the
/// pass/fail rules mirror the former replay.py 1:1.

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import type { Readable, Writable } from "node:stream";

// tools/ -> repo root.
const REPO_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));

// Server->client requests are answered with these results; anything not
// listed is answered with null.
const SERVER_REQUEST_DEFAULTS = new Map<string, unknown>([
    ["window/workDoneProgress/create", null],
    ["client/registerCapability", null],
    ["workspace/configuration", [{}]],
]);

interface TraceRecord {
    ts: number;
    msg: string;
}

/// Represent a value the way Python's str()/f-string would, so log lines
/// match: absent/null renders as "None".
function pyRepr(v: unknown): string {
    if (v === undefined || v === null) {
        return "None";
    }
    if (typeof v === "string") {
        return v;
    }
    if (typeof v === "number" || typeof v === "boolean" || typeof v === "bigint") {
        return String(v);
    }
    return JSON.stringify(v);
}

function asObject(v: unknown): Record<string, unknown> {
    return typeof v === "object" && v !== null ? (v as Record<string, unknown>) : {};
}

function stringField(obj: Record<string, unknown>, key: string): string | undefined {
    const v = obj[key];
    return typeof v === "string" ? v : undefined;
}

/// Split like Python's str.splitlines(): break on \r\n, \r and \n, and drop
/// the trailing empty element a final line break would otherwise produce.
function splitLines(text: string): string[] {
    const lines = text.split(/\r\n|\r|\n/);
    if (lines.length > 0 && lines[lines.length - 1] === "") {
        lines.pop();
    }
    return lines;
}

/// Percent-decode a string the lenient way urllib.parse.unquote does:
/// %XX runs are decoded as UTF-8; anything that is not a valid escape is
/// kept verbatim.
function unquote(s: string): string {
    const out: string[] = [];
    let byteRun: number[] = [];
    const flush = (): void => {
        if (byteRun.length > 0) {
            out.push(Buffer.from(byteRun).toString("utf-8"));
            byteRun = [];
        }
    };
    let i = 0;
    while (i < s.length) {
        if (s.charAt(i) === "%" && /^[0-9A-Fa-f]{2}$/.test(s.slice(i + 1, i + 3))) {
            byteRun.push(Number.parseInt(s.slice(i + 1, i + 3), 16));
            i += 3;
        } else {
            flush();
            out.push(s.charAt(i));
            i += 1;
        }
    }
    flush();
    return out.join("");
}

/// Percent-encode like urllib.parse.quote(string, safe): always-safe chars
/// are ASCII letters, digits and "_.-~"; `safe` names extra chars to leave
/// untouched; everything else becomes uppercase %XX over the UTF-8 bytes.
function quote(s: string, safe: string): string {
    const alwaysSafe = /[A-Za-z0-9_.\-~]/;
    let out = "";
    for (const b of Buffer.from(s, "utf-8")) {
        const c = String.fromCharCode(b);
        if (b < 128 && (alwaysSafe.test(c) || safe.includes(c))) {
            out += c;
        } else {
            out += "%" + b.toString(16).toUpperCase().padStart(2, "0");
        }
    }
    return out;
}

/// Load a JSONL trace file. Each line: {"ts": <ms>, "msg": "<json>"}
function loadTrace(p: string): TraceRecord[] {
    return splitLines(fs.readFileSync(p, "utf-8"))
        .filter((line) => line.trim() !== "")
        .map((line) => JSON.parse(line) as TraceRecord);
}

function extractOriginalWorkspace(records: TraceRecord[]): string | null {
    for (const rec of records) {
        const parsed = asObject(JSON.parse(rec.msg) as unknown);
        if (stringField(parsed, "method") !== "initialize") {
            continue;
        }
        const rootUri = stringField(asObject(parsed["params"]), "rootUri");
        if (!rootUri?.startsWith("file://")) {
            continue;
        }
        let workspace = unquote(rootUri.slice("file://".length));
        if (workspace.length > 2 && workspace.startsWith("/") && workspace[2] === ":") {
            workspace = workspace.slice(1);
        }
        return workspace;
    }
    return null;
}

/// Map recorded workspace path to the current repo location.
function rewriteWorkspace(originalWs: string): string | null {
    for (const marker of ["tests/data/", "tests/smoke/"]) {
        const idx = originalWs.indexOf(marker);
        if (idx !== -1) {
            return path.join(REPO_ROOT, originalWs.slice(idx));
        }
    }
    return null;
}

/// Text-level workspace path replacement in all messages.
///
/// Backslashes in Windows paths must be doubled inside JSON strings.
function rewriteRecords(records: TraceRecord[], originalWs: string, newWs: string): TraceRecord[] {
    const newWsJson = newWs.replaceAll("\\", "\\\\");
    const originalEncoded = quote(originalWs, "/:");
    const newEncoded = quote(newWs, "/:");
    const newEncodedJson = newEncoded.replaceAll("\\", "\\\\");
    return records.map((rec) => {
        let msg = rec.msg.replaceAll(originalWs, newWsJson);
        if (originalEncoded !== originalWs) {
            msg = msg.replaceAll(originalEncoded, newEncodedJson);
        }
        return { ts: rec.ts, msg };
    });
}

function printTraceInfo(name: string, records: TraceRecord[], workspace: string | null): void {
    const methods = new Map<string, number>();
    for (const rec of records) {
        const m = stringField(asObject(JSON.parse(rec.msg) as unknown), "method");
        if (m !== undefined && m !== "") {
            methods.set(m, (methods.get(m) ?? 0) + 1);
        }
    }
    console.log(`--- ${name} ---`);
    console.log(`  messages: ${records.length}`);
    if (workspace !== null && workspace !== "") {
        console.log(`  workspace: ${workspace}`);
    }
    const first = records[0];
    const last = records[records.length - 1];
    if (first !== undefined && last !== undefined) {
        console.log(`  duration: ${((last.ts - first.ts) / 1000).toFixed(1)}s`);
    }
    // Stable descending sort by count keeps first-seen order for ties, like
    // Python's sorted(..., key=lambda x: -x[1]) over an insertion-ordered dict.
    const top = [...methods.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8);
    console.log(`  methods: ${top.map(([m, n]) => `${m}(${n})`).join(", ")}`);
}

class TimeoutError extends Error {}

/// Reject with a TimeoutError after `ms`; otherwise settle with `promise`.
function withTimeout<T>(promise: Promise<T>, ms: number): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => {
            reject(new TimeoutError());
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

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/// Frame and write a single LSP message atomically (header + body in one
/// write so concurrent writers never interleave bytes). Resolves once the
/// bytes are flushed; rejects on a broken pipe.
function writeLspMessage(stdin: Writable, payload: string): Promise<void> {
    const encoded = Buffer.from(payload, "utf-8");
    const header = Buffer.from(`Content-Length: ${encoded.length}\r\n\r\n`, "ascii");
    return new Promise<void>((resolve, reject) => {
        stdin.write(Buffer.concat([header, encoded]), (err) => {
            if (err) {
                reject(err);
            } else {
                resolve();
            }
        });
    });
}

/// Incrementally parse LSP messages off a stream. read() yields the next
/// message, or null at EOF (or on a header with no Content-Length, matching
/// read_lsp_message returning None).
class LspMessageReader {
    private buffer = Buffer.alloc(0);
    private messages: (Record<string, unknown> | null)[] = [];
    private waiters: ((m: Record<string, unknown> | null) => void)[] = [];
    private done = false;

    constructor(stream: Readable) {
        stream.on("data", (chunk: Buffer) => {
            this.buffer = Buffer.concat([this.buffer, chunk]);
            this.parse();
        });
        stream.on("end", () => {
            this.finish();
        });
        stream.on("close", () => {
            this.finish();
        });
        stream.on("error", () => {
            this.finish();
        });
    }

    private emit(m: Record<string, unknown> | null): void {
        const waiter = this.waiters.shift();
        if (waiter) {
            waiter(m);
        } else {
            this.messages.push(m);
        }
    }

    private finish(): void {
        if (this.done) {
            return;
        }
        this.done = true;
        while (this.waiters.length > 0) {
            const waiter = this.waiters.shift();
            if (waiter) {
                waiter(null);
            }
        }
    }

    private parse(): void {
        while (!this.done) {
            const sep = this.buffer.indexOf("\r\n\r\n");
            if (sep === -1) {
                return;
            }
            const header = this.buffer.subarray(0, sep).toString("ascii");
            const match = /Content-Length:\s*(\d+)/.exec(header);
            const digits = match?.[1];
            if (digits === undefined) {
                this.finish();
                return;
            }
            const length = Number.parseInt(digits, 10);
            const bodyStart = sep + 4;
            if (this.buffer.length < bodyStart + length) {
                return;
            }
            const body = this.buffer.subarray(bodyStart, bodyStart + length);
            this.buffer = this.buffer.subarray(bodyStart + length);
            try {
                this.emit(asObject(JSON.parse(body.toString("utf-8")) as unknown));
            } catch {
                this.finish();
                return;
            }
        }
    }

    read(): Promise<Record<string, unknown> | null> {
        const queued = this.messages.shift();
        if (queued !== undefined) {
            return Promise.resolve(queued);
        }
        if (this.done) {
            return Promise.resolve(null);
        }
        return new Promise((resolve) => {
            this.waiters.push(resolve);
        });
    }
}

interface Pending {
    promise: Promise<void>;
    resolve: () => void;
}

function makePending(): Pending {
    let resolve: () => void = () => undefined;
    const promise = new Promise<void>((r) => {
        resolve = r;
    });
    return { promise, resolve };
}

/// Replay a single trace. Returns true=PASS, false=FAIL, null=SKIP.
async function replayOne(
    tracePath: string,
    cliceBin: string,
    timeout: number,
    wallTimeout: number,
): Promise<boolean | null> {
    let records = loadTrace(tracePath);
    const name = path.basename(tracePath);
    if (records.length === 0) {
        console.log(`SKIP: ${name} (empty)`);
        return null;
    }

    const originalWs = extractOriginalWorkspace(records);
    let displayWs = originalWs;
    if (originalWs !== null) {
        const newWs = rewriteWorkspace(originalWs);
        if (newWs !== null && originalWs !== newWs) {
            records = rewriteRecords(records, originalWs, newWs);
            displayWs = newWs;
        }
    }

    printTraceInfo(name, records, displayWs);

    const env = { ...process.env };
    if (process.platform === "darwin") {
        const prev = env["ASAN_OPTIONS"] ?? "";
        env["ASAN_OPTIONS"] = prev ? `${prev}:detect_leaks=0` : "detect_leaks=0";
    }

    const proc = spawn(cliceBin, ["serve"], { env });
    const stdin = proc.stdin;
    const reader = new LspMessageReader(proc.stdout);

    const stderrChunks: Buffer[] = [];
    proc.stderr.on("data", (d: Buffer) => {
        stderrChunks.push(d);
    });
    const stderrEnded = new Promise<void>((resolve) => {
        proc.stderr.once("end", () => {
            resolve();
        });
        proc.stderr.once("close", () => {
            resolve();
        });
    });
    const exited = new Promise<void>((resolve) => {
        proc.once("exit", () => {
            resolve();
        });
    });

    const pending = new Map<unknown, Pending>();
    const initIds = new Set<unknown>();
    const initErrors: string[] = [];
    const anomalies: string[] = [];

    const readerLoop = async (): Promise<void> => {
        try {
            for (;;) {
                const msg = await reader.read();
                if (msg === null) {
                    break;
                }
                const idRaw = msg["id"];
                const hasId = idRaw !== undefined && idRaw !== null;
                const method = stringField(msg, "method");
                if (method === "window/logMessage") {
                    const text = stringField(asObject(msg["params"]), "message") ?? "";
                    if (text.includes("[anomaly:")) {
                        anomalies.push(text);
                        console.log(`  ANOMALY: ${text}`);
                    }
                }
                if (hasId && method !== undefined) {
                    const resp = SERVER_REQUEST_DEFAULTS.has(method)
                        ? (SERVER_REQUEST_DEFAULTS.get(method) ?? null)
                        : null;
                    await writeLspMessage(
                        stdin,
                        JSON.stringify({ jsonrpc: "2.0", id: idRaw, result: resp }),
                    );
                } else if (hasId) {
                    const entry = pending.get(idRaw);
                    pending.delete(idRaw);
                    if (entry) {
                        entry.resolve();
                    }
                    if ("error" in msg) {
                        const err = asObject(msg["error"]);
                        console.log(
                            `  ERROR response id=${pyRepr(idRaw)}: ` +
                                `code=${pyRepr(err["code"])}, message=${pyRepr(err["message"])}`,
                        );
                        if (initIds.has(idRaw)) {
                            initErrors.push(
                                `initialize rejected: code=${pyRepr(err["code"])}, ` +
                                    `message=${pyRepr(err["message"])}`,
                            );
                        }
                    }
                }
            }
        } catch {
            // Broken pipe / read error: end quietly, like the Python reader.
        } finally {
            for (const entry of pending.values()) {
                entry.resolve();
            }
        }
    };

    const wallStart = performance.now();
    const wallDeadline = wallStart + wallTimeout * 1000;
    const remainingWall = (): number => Math.max(0, (wallDeadline - performance.now()) / 1000);
    const elapsed = (): string => ((performance.now() - wallStart) / 1000).toFixed(1);

    const readerPromise = readerLoop();
    let success = true;
    let lastMethod: string | undefined;
    let sentCount = 0;

    const gatherPending = (): Promise<unknown[]> =>
        Promise.all([...pending.values()].map((p) => p.promise));

    try {
        for (let i = 0; i < records.length; i++) {
            const rec = records[i];
            if (rec === undefined) {
                continue;
            }
            if (remainingWall() <= 0) {
                console.log(
                    `  result: TIMEOUT (wall-clock ${wallTimeout}s exceeded, ${elapsed()}s)`,
                );
                success = false;
                break;
            }

            if (i > 0) {
                const prev = records[i - 1];
                const delay = prev === undefined ? 0 : rec.ts - prev.ts;
                if (delay > 0) {
                    await sleep(delay);
                }
            }

            const parsed = asObject(JSON.parse(rec.msg) as unknown);
            const method = stringField(parsed, "method");
            const idRaw = parsed["id"];
            const hasId = idRaw !== undefined && idRaw !== null;
            lastMethod = method !== undefined && method !== "" ? method : lastMethod;

            // Before shutdown/exit, wait for all pending responses.
            if ((method === "shutdown" || method === "exit") && pending.size > 0) {
                try {
                    await withTimeout(gatherPending(), Math.min(timeout, remainingWall()) * 1000);
                } catch (e) {
                    if (e instanceof TimeoutError) {
                        console.log(
                            `  result: HANG (${pending.size} pending before ${method}, ${elapsed()}s)`,
                        );
                        success = false;
                        break;
                    }
                    throw e;
                }
                pending.clear();
            }

            if (hasId && method !== undefined) {
                pending.set(idRaw, makePending());
                if (method === "initialize") {
                    initIds.add(idRaw);
                }
            }

            try {
                await withTimeout(
                    writeLspMessage(stdin, rec.msg),
                    Math.min(30, remainingWall()) * 1000,
                );
            } catch (e) {
                if (e instanceof TimeoutError) {
                    console.log(
                        `  result: HANG (write blocked at ${pyRepr(lastMethod)},` +
                            ` sent=${sentCount}/${records.length}, ${elapsed()}s)`,
                    );
                    success = false;
                    break;
                }
                throw e;
            }
            sentCount = i + 1;
        }
    } catch {
        // Broken pipe while sending: the server died mid-stream.
        await waitExitOrKill(proc, exited, 5000);
        const returncode = exitStatus(proc);
        console.log(
            `  result: CRASH (broken pipe at ${pyRepr(lastMethod)}, exit=${pyRepr(returncode)},` +
                ` sent=${sentCount}/${records.length}, ${elapsed()}s)`,
        );
        success = false;
    }

    // Wait for remaining responses after all messages sent.
    if (success && pending.size > 0) {
        try {
            await withTimeout(gatherPending(), Math.min(timeout, remainingWall()) * 1000);
        } catch (e) {
            if (e instanceof TimeoutError) {
                console.log(`  result: HANG (${pending.size} pending after ${elapsed()}s)`);
                success = false;
            } else {
                throw e;
            }
        }
    }

    if (success && initErrors.length > 0) {
        console.log(`  result: FAIL (${initErrors[0] ?? ""})`);
        success = false;
    }

    if (success) {
        try {
            stdin.end();
            await withTimeout(
                new Promise<void>((resolve) => {
                    stdin.once("close", () => {
                        resolve();
                    });
                }),
                5000,
            );
        } catch {
            // Already closed or a broken pipe; the exit handling below decides.
        }
    }

    await waitExitOrKill(proc, exited, 10000);
    await withTimeout(readerPromise, 2000).catch(() => undefined);

    await withTimeout(stderrEnded, 2000).catch(() => undefined);
    const stderrData = Buffer.concat(stderrChunks);
    const returncode = exitStatus(proc);
    const signalName = proc.signalCode;

    const printStderr = (): void => {
        if (stderrData.length > 0) {
            for (const line of splitLines(stderrData.toString("utf-8")).slice(-20)) {
                console.log(`  | ${line}`);
            }
        }
    };

    if (!success) {
        printStderr();
        return false;
    }

    if (returncode !== null && returncode !== 0) {
        const sig = signalName !== null ? ` (${signalName})` : "";
        console.log(`  result: CRASH (exit=${returncode}${sig}, ${elapsed()}s)`);
        printStderr();
        return false;
    }

    if (anomalies.length > 0) {
        // Anomalies are internal clice bugs; a replay session must be clean.
        console.log(`  result: FAIL (${anomalies.length} anomaly report(s), ${elapsed()}s)`);
        printStderr();
        return false;
    }

    console.log(`  result: PASS (${elapsed()}s)`);
    return true;
}

/// Python returncode: exit code when the process exited, or -signal when a
/// signal killed it (null while still running).
function exitStatus(proc: {
    exitCode: number | null;
    signalCode: NodeJS.Signals | null;
}): number | null {
    if (proc.signalCode !== null) {
        return -os.constants.signals[proc.signalCode];
    }
    return proc.exitCode;
}

async function waitExitOrKill(
    proc: {
        exitCode: number | null;
        signalCode: NodeJS.Signals | null;
        kill: (s: NodeJS.Signals) => boolean;
    },
    exited: Promise<void>,
    ms: number,
): Promise<void> {
    if (proc.exitCode !== null || proc.signalCode !== null) {
        return;
    }
    try {
        await withTimeout(exited, ms);
    } catch {
        proc.kill("SIGKILL");
        await exited;
    }
}

interface Args {
    traces: string[];
    clice: string;
    timeout: number;
    wallTimeout: number;
}

function usageError(message: string): never {
    console.error(`replay.ts: error: ${message}`);
    console.error(
        "usage: replay.ts [-h] --clice CLICE [--timeout TIMEOUT] [--wall-timeout WALL_TIMEOUT] traces [traces ...]",
    );
    process.exit(2);
}

function parseIntArg(value: string | undefined, flag: string): number {
    if (value === undefined) {
        usageError(`argument ${flag}: expected one argument`);
    }
    const n = Number.parseInt(value, 10);
    if (Number.isNaN(n) || !/^[+-]?\d+$/.test(value.trim())) {
        usageError(`argument ${flag}: invalid int value: '${value}'`);
    }
    return n;
}

function parseArgs(argv: string[]): Args {
    const traces: string[] = [];
    let clice: string | undefined;
    let timeout = 120;
    let wallTimeout = 300;

    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === undefined) {
            break;
        }
        if (a === "-h" || a === "--help") {
            console.log(
                "usage: replay.ts [-h] --clice CLICE [--timeout TIMEOUT] [--wall-timeout WALL_TIMEOUT] traces [traces ...]",
            );
            process.exit(0);
        } else if (a === "--clice") {
            clice = argv[++i];
            if (clice === undefined) {
                usageError("argument --clice: expected one argument");
            }
        } else if (a.startsWith("--clice=")) {
            clice = a.slice("--clice=".length);
        } else if (a === "--timeout") {
            timeout = parseIntArg(argv[++i], "--timeout");
        } else if (a.startsWith("--timeout=")) {
            timeout = parseIntArg(a.slice("--timeout=".length), "--timeout");
        } else if (a === "--wall-timeout") {
            wallTimeout = parseIntArg(argv[++i], "--wall-timeout");
        } else if (a.startsWith("--wall-timeout=")) {
            wallTimeout = parseIntArg(a.slice("--wall-timeout=".length), "--wall-timeout");
        } else if (a.startsWith("-") && a !== "-") {
            usageError(`unrecognized arguments: ${a}`);
        } else {
            traces.push(a);
        }
    }

    if (traces.length === 0) {
        usageError("the following arguments are required: traces");
    }
    if (clice === undefined) {
        usageError("the following arguments are required: --clice");
    }
    return { traces, clice, timeout, wallTimeout };
}

async function main(): Promise<number> {
    const args = parseArgs(process.argv.slice(2));
    let passed = 0;
    let failed = 0;
    let skipped = 0;

    for (const trace of args.traces) {
        if (!fs.existsSync(trace)) {
            console.log(`SKIP: ${trace} (not found)`);
            skipped += 1;
            continue;
        }
        const result = await replayOne(trace, args.clice, args.timeout, args.wallTimeout);
        if (result === null) {
            skipped += 1;
        } else if (result) {
            passed += 1;
        } else {
            failed += 1;
        }
    }

    const total = passed + failed + skipped;
    let summary = `\n${passed}/${total} passed`;
    if (skipped > 0) {
        summary += `, ${skipped} skipped`;
    }
    if (failed > 0) {
        summary += `, ${failed} FAILED`;
    }
    console.log(summary);
    return failed > 0 ? 1 : 0;
}

process.exit(await main());
