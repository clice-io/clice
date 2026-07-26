/// Helpers for the agentic side channel: a minimal JSON-RPC client that
/// speaks Content-Length framing over TCP, plus the `clice query` CLI
/// runners used by the CLI-facing tests.

import { execFile } from "node:child_process";
import * as net from "node:net";
import * as path from "node:path";

export interface RpcResponse<T> {
    result?: T;
    error?: unknown;
}

/// JSON.stringify that tolerates the BigInt symbol ids in parsed responses,
/// for use in assertion failure messages.
export function jsonSafe(value: unknown): string {
    return JSON.stringify(value, (_key, v: unknown) => (typeof v === "bigint" ? v.toString() : v));
}

/// Parse a JSON-RPC frame, preserving 64-bit symbol ids. uint64 symbolId
/// values exceed JS's safe integer range, so quote them before JSON.parse
/// and restore them as BigInt in the reviver — otherwise a search -> id ->
/// lookup roundtrip loses precision and finds nothing. The responses are
/// machine-generated, so a "symbolId": key never collides with digits inside
/// a string value.
function parseLossless(text: string): unknown {
    const guarded = text.replace(/("symbolId"\s*:\s*)(\d{16,})/g, '$1"$2"');
    return JSON.parse(guarded, (key, value: unknown) =>
        key === "symbolId" && typeof value === "string" ? BigInt(value) : value,
    );
}

/// Serialize request params, emitting BigInt symbol ids as bare integer
/// literals (JSON.stringify rejects BigInt outright).
function stringifyLossless(value: unknown): string {
    if (typeof value === "bigint") {
        return value.toString();
    }
    if (value === null) {
        return "null";
    }
    switch (typeof value) {
        case "number":
        case "boolean":
            return String(value);
        case "string":
            return JSON.stringify(value);
        case "object": {
            if (Array.isArray(value)) {
                return `[${value.map(stringifyLossless).join(",")}]`;
            }
            const parts: string[] = [];
            for (const [k, v] of Object.entries(value)) {
                if (v !== undefined) {
                    parts.push(`${JSON.stringify(k)}:${stringifyLossless(v)}`);
                }
            }
            return `{${parts.join(",")}}`;
        }
        default:
            return "null";
    }
}

/// Minimal JSON-RPC client that speaks Content-Length framing over TCP.
/// Requests are issued one at a time (the tests are sequential), so a
/// single FIFO of waiters is enough to pair frames with their requests.
export class AgenticRpcClient {
    private socket = new net.Socket();
    private requestId = 0;
    private buffer = Buffer.alloc(0);
    private waiters: { resolve: (frame: string) => void; reject: (err: Error) => void }[] = [];

    connect(host: string, port: number): Promise<void> {
        return new Promise((resolve, reject) => {
            this.socket.once("error", reject);
            this.socket.on("data", (data: Buffer) => {
                this.onData(data);
            });
            // A close with requests outstanding (e.g. the server crashed)
            // must reject them, not leave their promises hanging until the
            // test timeout.
            this.socket.on("close", () => {
                this.failWaiters(new Error("agentic socket closed with a request outstanding"));
            });
            this.socket.on("error", (err) => {
                this.failWaiters(err instanceof Error ? err : new Error(String(err)));
            });
            this.socket.connect(port, host, () => {
                this.socket.off("error", reject);
                resolve();
            });
        });
    }

    private failWaiters(err: Error): void {
        const waiters = this.waiters;
        this.waiters = [];
        waiters.forEach((waiter) => {
            waiter.reject(err);
        });
    }

    private onData(data: Buffer): void {
        this.buffer = Buffer.concat([this.buffer, data]);
        while (this.waiters.length > 0) {
            const frame = this.tryParseFrame();
            if (frame === null) {
                return;
            }
            const waiter = this.waiters.shift();
            waiter?.resolve(frame);
        }
    }

    private tryParseFrame(): string | null {
        const sep = this.buffer.indexOf("\r\n\r\n");
        if (sep < 0) {
            return null;
        }
        const headers = this.buffer.subarray(0, sep).toString("utf8");
        let contentLength = 0;
        for (const line of headers.split("\r\n")) {
            if (line.toLowerCase().startsWith("content-length:")) {
                contentLength = Number.parseInt(line.slice(line.indexOf(":") + 1).trim(), 10);
            }
        }
        const total = sep + 4 + contentLength;
        if (this.buffer.length < total) {
            return null;
        }
        const body = this.buffer.subarray(sep + 4, total).toString("utf8");
        this.buffer = this.buffer.subarray(total);
        return body;
    }

    request<T = unknown>(method: string, params: Record<string, unknown>): Promise<RpcResponse<T>> {
        this.requestId += 1;
        const body = stringifyLossless({
            jsonrpc: "2.0",
            id: this.requestId,
            method,
            params,
        });
        return new Promise((resolve, reject) => {
            this.waiters.push({
                resolve: (frame) => {
                    resolve(parseLossless(frame) as RpcResponse<T>);
                },
                reject,
            });
            this.socket.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
        });
    }

    /// Send a notification (no id, no response expected).
    notify(method: string, params: Record<string, unknown>): void {
        const body = stringifyLossless({ jsonrpc: "2.0", method, params });
        this.socket.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
    }

    close(): void {
        // end() flushes queued writes (a just-sent notify) before FIN;
        // destroy() could drop them and hang the exit gate.
        this.socket.end();
    }
}

export interface QueryResult {
    status: number | null;
    stdout: string;
    stderr: string;
}

/// Run `clice query`, capturing exit status and output without throwing on
/// a non-zero exit (the connection-refused case exits non-zero on purpose).
function runQuery(executable: string, args: string[], timeout = 10_000): Promise<QueryResult> {
    return new Promise((resolve) => {
        execFile(executable, args, { timeout, encoding: "utf8" }, (err, stdout, stderr) => {
            let status: number | null;
            if (err === null) {
                status = 0;
            } else if (typeof err.code === "number") {
                status = err.code;
            } else {
                status = null; // killed (e.g. timeout) or spawn failure
            }
            resolve({ status, stdout, stderr });
        });
    });
}

/// `clice query --host H --port P --path PATH` — resolve a compile command.
export function runAgentic(
    executable: string,
    host: string,
    port: number,
    filePath: string,
    timeout = 10_000,
): Promise<QueryResult> {
    return runQuery(
        executable,
        ["query", "--host", host, "--port", String(port), "--path", filePath],
        timeout,
    );
}

/// `clice query --host H --port P --method M --k v ...`.
export function runCli(
    executable: string,
    host: string,
    port: number,
    method: string,
    kwargs: Record<string, string | number> = {},
): Promise<QueryResult> {
    const args = ["query", "--host", host, "--port", String(port), "--method", method];
    for (const [key, value] of Object.entries(kwargs)) {
        args.push(`--${key}`, String(value));
    }
    return runQuery(executable, args);
}

/// Forward-slash form of a path; the agentic side channel speaks POSIX
/// paths on every platform.
export function posix(p: string): string {
    return p.split(path.sep).join("/");
}
