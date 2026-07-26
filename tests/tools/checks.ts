/// Observation and assertion helpers: diagnostics, anomalies, waits.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { withTimeout, type CliceClient } from "./client.ts";

// Standard timing constants — use these instead of hardcoded sleep values.
export const MTIME_GRANULARITY = 1_100; // Filesystem mtime precision + margin
export const SETTLE_TIME = 500; // Server stabilization after an operation
export const IDLE_TIMEOUT = 5_000; // Idle soak time in lifecycle tests

export function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/// Normalize a definition/references response to a list of Locations.
export function locationsOf<T>(result: T | T[] | null | undefined): T[] {
    if (result === null || result === undefined) {
        return [];
    }
    return Array.isArray(result) ? result : [result];
}

/// Trigger recompilation via hover and wait for fresh diagnostics.
/// Useful after didChange or on-disk file modifications.
export async function waitForRecompile(
    client: CliceClient,
    uri: string,
    timeout = 60_000,
): Promise<void> {
    const arrived = client.armDiagnostics(uri);
    await client.hoverAt(uri, 0, 0);
    await withTimeout(arrived, timeout, `diagnostics ${uri}`);
}

/// Poll workspace/symbol until a specific symbol appears in the index.
export async function waitForIndex(
    client: CliceClient,
    uri: string,
    symbolName = "add",
    timeoutSeconds = 30,
): Promise<boolean> {
    await client.hoverAt(uri, 0, 0);
    for (let i = 0; i < timeoutSeconds; i++) {
        const result = await client.workspaceSymbols(symbolName);
        if (result?.some((s) => s.name === symbolName)) {
            return true;
        }
        await sleep(1_000);
    }
    return false;
}

/// URIs of the references at a position (declaration excluded).
export async function referenceUris(
    client: CliceClient,
    uri: string,
    line: number,
    character: number,
): Promise<string[]> {
    const refs = await client.referencesAt(uri, line, character, {
        includeDeclaration: false,
    });
    return (refs ?? []).map((ref) => ref.uri);
}

/// Poll references at a position until expectedUri shows up.
export async function waitForReference(
    client: CliceClient,
    uri: string,
    line: number,
    character: number,
    expectedUri: string,
    timeoutSeconds = 30,
): Promise<boolean> {
    for (let i = 0; i < timeoutSeconds; i++) {
        if ((await referenceUris(client, uri, line, character)).includes(expectedUri)) {
            return true;
        }
        await sleep(1_000);
    }
    return false;
}

const ANOMALY_PATTERN = /\[anomaly:([A-Za-z]+)\]/;

/// Anomaly IDs from window/logMessage notifications (master process).
export function anomaliesInLogMessages(client: CliceClient): string[] {
    const found: string[] = [];
    for (const msg of client.logMessages) {
        const id = ANOMALY_PATTERN.exec(msg.message)?.[1];
        if (id !== undefined) {
            found.push(id);
        }
    }
    return found;
}

function logFiles(workspace: string | null): string[] {
    if (workspace === null) {
        return [];
    }
    const logsDir = path.join(workspace, ".clice", "logs");
    if (!fs.existsSync(logsDir)) {
        return [];
    }
    return fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".log"))
        .sort()
        .map((name) => path.join(logsDir, name));
}

/// Anomaly IDs from master/worker log files under <workspace>/.clice/logs.
/// Workers don't forward logMessage in v1.0, so their anomalies are only
/// visible in their log files.
export function anomaliesInLogFiles(workspace: string | null): string[] {
    const found: string[] = [];
    for (const logFile of logFiles(workspace)) {
        for (const line of fs.readFileSync(logFile, "utf8").split("\n")) {
            const match = ANOMALY_PATTERN.exec(line);
            if (match) {
                found.push(`${match[1]} (${path.basename(logFile)}: ${line.trim()})`);
            }
        }
    }
    return found;
}

const CRASH_TRACE_MARKER = "=== CRASH STACK TRACE ===";

/// Crash stack traces recorded by crashed processes in their log files.
export function crashTracesInLogFiles(workspace: string | null): string[] {
    const traces: string[] = [];
    for (const logFile of logFiles(workspace)) {
        const text = fs.readFileSync(logFile, "utf8");
        const pos = text.indexOf(CRASH_TRACE_MARKER);
        if (pos !== -1) {
            traces.push(`--- ${path.basename(logFile)} ---\n${text.slice(pos)}`);
        }
    }
    return traces;
}

/// Assert the session produced zero anomalies (client messages + logs).
/// Runs in every integration test teardown: anomalies are internal clice
/// bugs and must never occur on regular paths.
export function assertNoAnomaly(client: CliceClient, workspace: string | null = null): void {
    const found = [...anomaliesInLogMessages(client), ...anomaliesInLogFiles(workspace)];
    // A crashed worker leaves its stack trace in its own log file; surface
    // it here so a one-off CI crash is diagnosable from the test output.
    const traces = found.length > 0 ? crashTracesInLogFiles(workspace) : [];
    const detail = traces.length > 0 ? "\n" + traces.join("\n") : "";
    if (found.length > 0) {
        throw new Error(`clice reported internal anomalies: ${found.join(", ")}${detail}`);
    }
}

/// Guidance texts from window/logMessage notifications.
export function guidanceMessages(client: CliceClient): string[] {
    return client.logMessages
        .filter((msg) => msg.message.includes("[guidance]"))
        .map((msg) => msg.message);
}

export function getErrors(diagnostics: proto.Diagnostic[]): proto.Diagnostic[] {
    return diagnostics.filter((d) => d.severity === proto.DiagnosticSeverity.Error);
}

export function assertNoErrors(client: CliceClient, uri: string, msg = ""): void {
    const errors = getErrors(client.diagnostics.get(uri) ?? []);
    if (errors.length > 0) {
        throw new Error(
            msg
                ? `${msg}: ${JSON.stringify(errors)}`
                : `Expected no errors, got: ${JSON.stringify(errors)}`,
        );
    }
}

export function assertHasErrors(client: CliceClient, uri: string, msg = ""): void {
    const errors = getErrors(client.diagnostics.get(uri) ?? []);
    if (errors.length === 0) {
        throw new Error(msg || "Expected at least one error diagnostic");
    }
}

export function assertDiagnosticsCount(
    client: CliceClient,
    uri: string,
    count: number,
    severity?: number,
): void {
    let diags = client.diagnostics.get(uri) ?? [];
    if (severity !== undefined) {
        diags = diags.filter((d) => d.severity === severity);
    }
    if (diags.length !== count) {
        throw new Error(
            `Expected ${count} diagnostics (severity=${severity}), ` +
                `got ${diags.length}: ${JSON.stringify(diags)}`,
        );
    }
}

export function assertCleanCompile(client: CliceClient, uri: string): void {
    const diags = client.diagnostics.get(uri) ?? [];
    if (diags.length > 0) {
        throw new Error(`Expected clean compile, got: ${JSON.stringify(diags)}`);
    }
}
