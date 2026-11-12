/// What tests read from the logs: never the narrative, only the faults a
/// test did not declare, and the excerpt a failure prints; and the logs
/// CLICE_KEEP_LOGS keeps of the cases that failed.

import * as fs from "node:fs";
import * as path from "node:path";
import { namesFile } from "../../client/workspace.ts";
import {
    logFaults,
    logFiles,
    logLevel,
    readLogLines,
    sessionLogs,
    sessionsUnder,
    textLines,
    type LogFault,
    type LogLine,
} from "../../process_gate.ts";

/// A process a test started, as the gate reads its logs: the session
/// directories it wrote, found by its pid anywhere under the copy, or the
/// stderr of one that wrote none (a server mirrors its log there, a
/// command its warnings and errors).
export interface Started {
    /// What a failure calls it.
    label: string;
    pid: number | undefined;
    stderr: string;
}

/// A crash or kill a test provokes in the worker running `request` on
/// `file`.
export interface FaultSite {
    request: string;
    file: string;
}

/// What a server of the test, or a command, was told to cause.
export interface Faults {
    crash?: FaultSite;
    kill?: FaultSite & { taken: boolean };
    anomalies?: true;
    /// Workers the test killed from outside.
    killed?: number;
}

/// What a test declared about its processes' logs: the error lines it
/// provokes and the faults of each server and command it started.
export interface Declared {
    errors?: readonly RegExp[];
    servers?: readonly Faults[];
    commands?: readonly Faults[];
}

/// The lines the processes a test started logged: each one's sessions,
/// each file read once, or else its stderr.
export function startedLines(root: string, started: readonly Started[]): LogLine[] {
    const sessions = sessionsUnder(root);
    const own: string[] = [];
    const streams: LogLine[] = [];
    for (const process of started) {
        const dirs = process.pid === undefined ? undefined : sessions.get(process.pid);
        if (dirs === undefined) {
            streams.push(...textLines(process.stderr, `${process.label} (stderr)`));
        } else {
            own.push(...dirs);
        }
    }
    return [...readLogLines(sessionLogs(own)), ...streams];
}

function shown(line: LogLine): string {
    return `${path.basename(line.file)}: ${line.text}`;
}

/// The faults the processes a test started logged, against what it
/// declared: each declared fault and error line happened, and nothing else
/// did. On failure, their warnings and errors.
export function gateLogs(
    root: string,
    started: readonly Started[],
    declared: Declared,
    failed: boolean,
): unknown[] {
    const failures: string[] = [];
    const lines = startedLines(root, started);
    const faults = logFaults(lines);
    const of = <K extends LogFault["kind"]>(kind: K) =>
        faults.filter((fault): fault is Extract<LogFault, { kind: K }> => fault.kind === kind);
    const all = [...(declared.servers ?? []), ...(declared.commands ?? [])];

    const crashes = of("crash");
    const own = crashes.filter((crash) => crash.worker === undefined);
    if (own.length > 0) {
        failures.push(`clice crashed:\n${own.map((crash) => shown(crash.line)).join("\n")}`);
    }
    const sites = all.flatMap(({ crash }) => (crash === undefined ? [] : [crash]));
    const declares = (site: FaultSite, crash: (typeof crashes)[number]) =>
        site.request === crash.request && namesFile(site.file, crash.file ?? "");
    const named = crashes.filter((crash) => crash.request !== undefined);
    const stray = named.filter((crash) => !sites.some((site) => declares(site, crash)));
    if (stray.length > 0) {
        failures.push(
            `worker crashes nothing declared:\n${stray.map((c) => `${c.request ?? ""} ${c.file ?? ""}`).join("\n")}`,
        );
    }
    const missed = sites.filter((site) => !named.some((crash) => declares(site, crash)));
    if (missed.length > 0) {
        failures.push(
            `declared crashes that never happened:\n${missed.map((c) => `${c.request} ${c.file}`).join("\n")}`,
        );
    }
    const kills = all.flatMap(({ kill }) => (kill === undefined ? [] : [kill]));
    const untaken = kills.filter((kill) => !kill.taken);
    if (untaken.length > 0) {
        failures.push(
            `declared kills no worker ran into:\n${untaken.map((kill) => `${kill.request} ${kill.file}`).join("\n")}`,
        );
    }
    const unnamed = crashes.filter(
        (crash) => crash.worker !== undefined && crash.request === undefined,
    );
    const explained =
        kills.length - untaken.length + all.reduce((sum, { killed }) => sum + (killed ?? 0), 0);
    if (unnamed.length !== explained) {
        failures.push(
            `the declared kills explain ${String(explained)} worker deaths naming no request; the logs have:\n${unnamed.map((crash) => shown(crash.line)).join("\n") || "(none)"}`,
        );
    }

    const anomalies = of("anomaly");
    if (anomalies.length > 0 && !all.some(({ anomalies }) => anomalies === true)) {
        failures.push(
            `anomalies nothing declared:\n${anomalies.map((anomaly) => shown(anomaly.line)).join("\n")}`,
        );
    }

    const errors = of("error");
    const patterns = declared.errors ?? [];
    const undeclared = errors.filter(
        ({ line }) => !patterns.some((pattern) => pattern.test(line.text)),
    );
    if (undeclared.length > 0) {
        failures.push(
            `error-level log lines the test did not declare:\n${undeclared.map(({ line }) => shown(line)).join("\n")}`,
        );
    }
    const unmatched = patterns.filter(
        (pattern) => !errors.some(({ line }) => pattern.test(line.text)),
    );
    if (unmatched.length > 0) {
        failures.push(
            `declared errors no error-level line matches:\n${unmatched.map((pattern) => pattern.source).join("\n")}`,
        );
    }

    if (failed || failures.length > 0) {
        console.log(`warnings and errors:\n${failureExcerpt(lines)}`);
    }
    return failures.map((failure) => new Error(failure));
}

/// The lines a failure prints: warnings and above, the last `limit` of them.
export function failureExcerpt(lines: readonly LogLine[], limit = 60): string {
    const picked = lines.filter((line) => {
        const level = logLevel(line.text)?.level;
        return level === "warning" || level === "error" || level === "fatal";
    });
    return picked.slice(-limit).map(shown).join("\n");
}

const keptLogs = process.env["CLICE_KEEP_LOGS"];
if (keptLogs !== undefined && !path.isAbsolute(keptLogs)) {
    throw new Error(
        `CLICE_KEEP_LOGS is ${JSON.stringify(keptLogs)}: it takes an absolute directory`,
    );
}

/// The directories keepLogs wrote, by case.
const kept = new Map<string, string[]>();

/// Under CLICE_KEEP_LOGS=<dir>, copy the logs of a case's copy to
/// <dir>/<case id>/<copy> before the copy goes; the runner drops a case's
/// once it passed (dropKeptLogs).
export function keepLogs(caseId: string, root: string): void {
    if (keptLogs === undefined) {
        return;
    }
    const copy = path.join(keptLogs, caseId, path.basename(root));
    for (const file of logFiles(root)) {
        const target = path.join(copy, path.relative(root, file));
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.copyFileSync(file, target);
    }
    kept.set(caseId, [...(kept.get(caseId) ?? []), copy]);
}

export function dropKeptLogs(caseId: string): void {
    for (const copy of kept.get(caseId) ?? []) {
        fs.rmSync(copy, { recursive: true, force: true });
    }
    kept.delete(caseId);
}
