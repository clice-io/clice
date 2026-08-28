import * as fs from "node:fs";
import * as path from "node:path";

/// Sanitizer/crash fingerprints scanned in server stderr.
export const SANITIZER_MARKERS = [
    "AddressSanitizer",
    "LeakSanitizer",
    "MemorySanitizer",
    "ThreadSanitizer",
    "UndefinedBehaviorSanitizer",
    "==ERROR:",
    "runtime error:",
] as const;

const ANOMALY_PATTERN = /\[anomaly:([A-Za-z]+)\]/;
const CRASH_TRACE_MARKER = "=== CRASH STACK TRACE ===";

export function logFiles(root: string | null, ignoreLogs?: ReadonlySet<string>): string[] {
    if (root === null) {
        return [];
    }
    const logsDir = path.join(root, ".clice", "logs");
    if (!fs.existsSync(logsDir)) {
        return [];
    }
    return fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".log"))
        .sort()
        .map((name) => path.join(logsDir, name))
        .filter((file) => !(ignoreLogs?.has(file) ?? false));
}

export function anomaliesInLogFiles(
    root: string | null,
    ignoreLogs?: ReadonlySet<string>,
): string[] {
    const found: string[] = [];
    for (const logFile of logFiles(root, ignoreLogs)) {
        for (const line of fs.readFileSync(logFile, "utf8").split("\n")) {
            const match = ANOMALY_PATTERN.exec(line);
            if (match) {
                found.push(`${match[1]} (${path.basename(logFile)}: ${line.trim()})`);
            }
        }
    }
    return found;
}

export function anomaliesInMessages(messages: string[]): string[] {
    return messages.flatMap((message) => {
        const id = ANOMALY_PATTERN.exec(message)?.[1];
        return id === undefined ? [] : [id];
    });
}

export function crashTracesInLogFiles(
    root: string | null,
    ignoreLogs?: ReadonlySet<string>,
): string[] {
    const traces: string[] = [];
    for (const logFile of logFiles(root, ignoreLogs)) {
        const text = fs.readFileSync(logFile, "utf8");
        const pos = text.indexOf(CRASH_TRACE_MARKER);
        if (pos !== -1) {
            traces.push(`--- ${path.basename(logFile)} ---\n${text.slice(pos)}`);
        }
    }
    return traces;
}

export function serverStderrExcerpt(stderrText: string): string {
    const interesting = stderrText
        .split("\n")
        .filter(
            (line) =>
                line.includes("[warn]") ||
                line.includes("[error]") ||
                line.includes("Sanitizer") ||
                line.includes("==ERROR:") ||
                line.includes("runtime error:"),
        );
    return interesting.slice(-80).join("\n");
}

export interface ProcessGateInput {
    exitCode: number | null;
    signalCode: string | null;
    stderrText: string;
    stderrComplete: boolean;
    stderrFailure?: string | undefined;
    stderrDrainedFromStart: boolean;
    sanitizerMarkerHit?: string | null | undefined;
}

export function processGateFailures(input: ProcessGateInput): string[] {
    const failures: string[] = [];
    if (input.signalCode !== null) {
        failures.push(`server exited from signal ${input.signalCode}`);
    } else if (input.exitCode !== 0) {
        failures.push(`server exited with code ${input.exitCode}`);
    }
    if (!input.stderrComplete) {
        failures.push(
            input.stderrFailure === undefined
                ? "stderr pump did not complete"
                : `stderr pump did not complete: ${input.stderrFailure}`,
        );
    }
    if (input.stderrDrainedFromStart && input.stderrText.includes("client not draining")) {
        failures.push("stderr mirror shed lines despite a draining client");
    }
    if (input.sanitizerMarkerHit) {
        failures.push(
            `server stderr contains sanitizer/runtime error output:\n${input.sanitizerMarkerHit}`,
        );
    } else if (SANITIZER_MARKERS.some((marker) => input.stderrText.includes(marker))) {
        failures.push("server stderr contains sanitizer/runtime error output");
    }
    return failures;
}

export function anomalyGateFailure(
    notifications: string[],
    root: string | null,
    ignoreLogs?: ReadonlySet<string>,
): string | null {
    const found = [...notifications, ...anomaliesInLogFiles(root, ignoreLogs)];
    if (found.length === 0) {
        return null;
    }
    const traces = crashTracesInLogFiles(root, ignoreLogs);
    const detail = traces.length > 0 ? "\n" + traces.join("\n") : "";
    return `clice reported internal anomalies: ${found.join(", ")}${detail}`;
}
