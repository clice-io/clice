/// What tests read from the logs: never the narrative, only the error
/// lines a test did not declare, and the excerpt a failure prints.

import * as fs from "node:fs";
import * as path from "node:path";
import { logFiles } from "../../process_gate.ts";

export interface LogLine {
    file: string;
    text: string;
}

/// A log line's level, from the `[level]` field after its timestamp.
export function levelOf(text: string): string | undefined {
    return /^\[[^\]]*\] \[(\w+)\]/.exec(text)?.[1];
}

export function readLogs(workspace: string): LogLine[] {
    const lines: LogLine[] = [];
    for (const file of logFiles(workspace)) {
        for (const text of fs.readFileSync(file, "utf8").split(/\r?\n/)) {
            if (text !== "") {
                lines.push({ file: path.basename(file), text });
            }
        }
    }
    return lines;
}

/// Error lines no pattern of `expected` matches.
export function undeclaredErrors(
    lines: readonly LogLine[],
    expected: readonly RegExp[],
): LogLine[] {
    return lines.filter((line) => {
        const level = levelOf(line.text);
        if (level !== "error" && level !== "critical") {
            return false;
        }
        return !expected.some((pattern) => pattern.test(line.text));
    });
}

/// The lines a failure prints: warnings and above, the last `limit` of them.
export function failureExcerpt(lines: readonly LogLine[], limit = 60): string {
    const picked = lines.filter((line) => {
        const level = levelOf(line.text);
        return level === "warning" || level === "error" || level === "critical";
    });
    return picked
        .slice(-limit)
        .map((line) => `${line.file}: ${line.text}`)
        .join("\n");
}
