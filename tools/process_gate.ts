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

export const ANOMALY_PATTERN = /\[anomaly:([A-Za-z]+)\]/;

/// The environment servers under test run in: every freshness answer given
/// without a look at the disk is checked against one, a contradiction
/// being an anomaly the gates catch.
export function serverEnv(): NodeJS.ProcessEnv {
    return { ...process.env, CLICE_SHADOW_FRESHNESS: "1" };
}

export function logFiles(root: string): string[] {
    const logsDir = path.join(root, ".clice", "logs");
    if (!fs.existsSync(logsDir)) {
        return [];
    }
    return fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".log"))
        .sort()
        .map((name) => path.join(logsDir, name));
}

export function anomaliesInLogFiles(root: string): string[] {
    const found: string[] = [];
    for (const logFile of logFiles(root)) {
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

/// What a C or C++ runtime prints as it aborts, which no logger tags: glibc's
/// own checks and assert(), macOS's assert(), the C++ runtimes' terminate,
/// LLVM's fatal errors.
const ABORT_PATTERNS = [
    /Fatal glibc error/,
    /\*\*\* .* \*\*\*: terminated/,
    /\b(?:free|malloc|realloc|calloc)\(\): /,
    /double free or corruption/,
    /Assertion `.*' failed/,
    /Assertion failed: .*, function /,
    /libc\+\+abi: /,
    /terminate called /,
    /LLVM ERROR: /,
];

export function serverStderrExcerpt(stderrText: string): string {
    const interesting = stderrText
        .split("\n")
        .filter(
            (line) =>
                line.includes("[warn]") ||
                line.includes("[error]") ||
                line.includes("Sanitizer") ||
                line.includes("==ERROR:") ||
                line.includes("runtime error:") ||
                ABORT_PATTERNS.some((pattern) => pattern.test(line)),
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
