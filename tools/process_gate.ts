import * as fs from "node:fs";
import * as path from "node:path";

/// Sanitizer/crash fingerprints scanned in server stderr: a report names
/// its sanitizer followed by ": ", ":DEADLYSIGNAL" or " has encountered",
/// which neither a logged path to LLVM's AddressSanitizer.cpp nor a name
/// qualified by its class does.
export const SANITIZER_MARKERS = [
    "AddressSanitizer: ",
    "LeakSanitizer: ",
    "MemorySanitizer: ",
    "ThreadSanitizer: ",
    "UndefinedBehaviorSanitizer: ",
    "Sanitizer:DEADLYSIGNAL",
    "Sanitizer has encountered a fatal error",
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

/// A session directory's name: when its process started and its pid,
/// `YYYY-MM-DD_HH-MM-SS_<pid>` (start_session_log).
const SESSION = /^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_(\d+)$/;

/// A path as clice spells it: through a Windows junction the drive reads
/// in lowercase.
function realPath(file: string): string | undefined {
    try {
        return fs.realpathSync(file).replace(/^[A-Z]:/, (drive) => drive.toLowerCase());
    } catch {
        return undefined;
    }
}

/// The session directories in a logging directory, oldest first: each a
/// process's, or a link to the session of a server that serves this
/// directory's project from another's (link_session_log).
export function sessionsIn(loggingDir: string): string[] {
    if (!fs.existsSync(loggingDir)) {
        return [];
    }
    return fs
        .readdirSync(loggingDir)
        .filter((name) => SESSION.test(name))
        .sort()
        .map((name) => path.join(loggingDir, name));
}

/// The session directories anywhere under `root`, by the pid of the
/// process that wrote each: a session and the links to it count once.
export function sessionsUnder(root: string): Map<number, string[]> {
    const found = new Map<number, string[]>();
    const seen = new Set<string>();
    const walk = (dir: string): void => {
        let entries: fs.Dirent[];
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        } catch {
            return;
        }
        for (const entry of entries.sort((a, b) => (a.name < b.name ? -1 : 1))) {
            const file = path.join(dir, entry.name);
            const pid = SESSION.exec(entry.name)?.[1];
            if (pid !== undefined && (entry.isDirectory() || entry.isSymbolicLink())) {
                const real = realPath(file);
                if (real !== undefined && !seen.has(real)) {
                    seen.add(real);
                    found.set(Number(pid), [...(found.get(Number(pid)) ?? []), file]);
                }
            } else if (entry.isDirectory()) {
                walk(file);
            }
        }
    };
    walk(root);
    return found;
}

/// The log files of `sessions`, each read once: a session reached through
/// two links is one. A rotated log (`master.1.log`) comes before the one
/// that goes on.
export function sessionLogs(sessions: readonly string[]): string[] {
    const seen = new Set<string>();
    return sessions.flatMap((session) =>
        (fs.existsSync(session) ? fs.readdirSync(session) : [])
            .filter((name) => name.endsWith(".log"))
            .sort()
            .map((name) => path.join(session, name))
            .filter((file) => {
                const real = realPath(file) ?? file;
                const fresh = !seen.has(real);
                seen.add(real);
                return fresh;
            }),
    );
}

/// Every log file the sessions under `root` wrote.
export function logFiles(root: string): string[] {
    return sessionLogs([...sessionsUnder(root).values()].flat());
}

/// A line clice logged, and where: a log file's path, or the stream of a
/// process that wrote no log file.
export interface LogLine {
    file: string;
    text: string;
}

/// The lines of log files, in file order.
export function readLogLines(files: readonly string[]): LogLine[] {
    return files.flatMap((file) => textLines(fs.readFileSync(file, "utf8"), file));
}

/// The non-empty lines of `text`, written to `file`.
export function textLines(text: string, file: string): LogLine[] {
    return text
        .split(/\r?\n/)
        .filter((line) => line !== "")
        .map((line) => ({ file, text: line }));
}

/// A log line's level and message, in either form clice writes: a log
/// file's and an editor's `[time] [level] [thread n] [file:line] message`,
/// a terminal's `level: message`. None for the lines a message of several
/// continues on, indented, nor for what is no log line.
export function logLevel(text: string): { level: string; message: string } | undefined {
    const match =
        /^\[[^\]]*\] \[(trace|debug|info|warning|error|fatal)\] (?:\[thread \d+\] )?(?:\[[^\]\s]+:\d+\] )?(.*)$/.exec(
            text,
        ) ??
        /^(?:\x1b\[[\d;]*m)?(trace|debug|info|warning|error|fatal):(?:\x1b\[0m)? (.*)$/.exec(text);
    return match === null ? undefined : { level: match[1] ?? "", message: match[2] ?? "" };
}

/// What went wrong in a run, as its logs tell it.
/// - `crash`: a worker died unasked (`worker`, and the request it ran when
///   it named one), or a process of clice crashed itself (no `worker`);
/// - `anomaly`: what clice reports on its anomaly channel, `[anomaly:Id]`;
/// - `error`: any other line at error level or above.
export type LogFault =
    | { kind: "crash"; worker?: string; request?: string; file?: string; line: LogLine }
    | { kind: "anomaly"; id: string; line: LogLine }
    | { kind: "error"; line: LogLine };

/// The stderr line a dying worker names its request in, as its supervisor
/// relays it: `[SF-0] clice worker crashed in: clice/worker/hover a.cpp`
/// (crashed_in_marker).
const CRASHED_IN = /^\[(\S+)\] clice worker crashed in: clice\/worker\/(\S+) (.*)$/;
/// The request a worker the pool killed for running too long ran.
const RAN = /^Worker (\S+) ran clice\/worker\/(\S+) (.*): killed after /;
/// The lines the supervisor quotes from a dead worker's log, one telling a
/// death, whose end follows unless past the anomaly's report limit.
const LAST_LINES = /^The last lines (\S+) logged before it died:/;
/// A failed wait for a worker's exit, which a death telling follows.
const LOST = /^Worker (\S+) lost: /;
/// A worker's death the pool did not ask for.
const WORKER_CRASH = /^\[anomaly:WorkerCrash\] Worker (\S+) /;
/// A death that is no crash: one asked from outside the session, or the
/// machine running out of memory.
const NOT_A_CRASH = /^Worker (\S+) (?:terminated by signal |ran out of memory at )/;
const ANOMALY = /^\[anomaly:(\w+)\] /;
/// A worker's own log, whose crash its supervisor tells.
const WORKER_LOG = /^S[FL]-\d+(?:\.1)?\.log$/;

/// The faults `lines` tell, in order. A worker's death is one crash,
/// whatever its supervisor and its own log say of it: the request it named
/// before it died, its last lines, how it ended. A crash header in a
/// supervisor's log or a process's stderr is that process's own crash.
export function logFaults(lines: readonly LogLine[]): LogFault[] {
    const faults: LogFault[] = [];
    interface Named {
        worker: string;
        request: string;
        file: string;
        line: LogLine;
        ran: boolean;
    }
    /// The requests a worker named since its last death.
    const named = new Map<string, Named>();
    /// A death its last lines told, waiting for the line telling its end.
    const dying = new Map<string, { worker: string; at: Named | undefined; line: LogLine }>();
    const lost = new Map<string, LogLine[]>();
    // A rotated log (`master.1.log`) goes on in the fresh one.
    const key = (line: LogLine, worker: string) =>
        `${line.file.replace(/\.1\.log$/, ".log")}\0${worker}`;
    const crash = (worker: string, at: Named | undefined, line: LogLine): LogFault =>
        at === undefined
            ? { kind: "crash", worker, line }
            : { kind: "crash", worker, request: at.request, file: at.file, line };
    // Past the anomaly's report limit, a death no line ends is a crash: told
    // by its last lines, or by the request it named alone.
    const unended = (k: string) => {
        const death = dying.get(k);
        if (death !== undefined) {
            faults.push(crash(death.worker, death.at, death.line));
            dying.delete(k);
            return;
        }
        const at = named.get(k);
        if (at !== undefined) {
            faults.push(crash(at.worker, at, at.line));
            named.delete(k);
        }
    };
    for (const line of lines) {
        const parsed = logLevel(line.text);
        if (parsed === undefined) {
            continue;
        }
        const { level, message } = parsed;
        const crashedIn = CRASHED_IN.exec(message);
        const ran = RAN.exec(message);
        const naming = crashedIn ?? ran;
        if (naming !== null) {
            const [, worker = "", request = "", file = ""] = naming;
            const k = key(line, worker);
            // A kill for running too long names each request it stopped.
            if (named.get(k)?.ran === true && ran !== null) {
                continue;
            }
            unended(k);
            named.set(k, { worker, request, file, line, ran: ran !== null });
            continue;
        }
        const last = LAST_LINES.exec(message);
        if (last !== null) {
            const worker = last[1] ?? "";
            const k = key(line, worker);
            const before = dying.get(k);
            if (before !== undefined) {
                faults.push(crash(worker, before.at, before.line));
            }
            dying.set(k, { worker, at: named.get(k), line });
            named.delete(k);
            lost.delete(k);
            continue;
        }
        const failedWait = LOST.exec(message);
        if (failedWait !== null) {
            const k = key(line, failedWait[1] ?? "");
            lost.set(k, [...(lost.get(k) ?? []), line]);
            continue;
        }
        const crashed = WORKER_CRASH.exec(message);
        const died = crashed ?? NOT_A_CRASH.exec(message);
        if (died !== null) {
            const worker = died[1] ?? "";
            const k = key(line, worker);
            if (crashed !== null) {
                faults.push(crash(worker, dying.get(k)?.at ?? named.get(k), line));
            }
            dying.delete(k);
            named.delete(k);
            lost.delete(k);
            continue;
        }
        if (level === "fatal" && message === "clice crashed") {
            if (!WORKER_LOG.test(path.basename(line.file))) {
                faults.push({ kind: "crash", line });
            }
            continue;
        }
        const anomaly = ANOMALY.exec(message);
        if (anomaly !== null) {
            // The logger's note that it reports this anomaly no more.
            if (!message.includes(" report limit (")) {
                faults.push({ kind: "anomaly", id: anomaly[1] ?? "", line });
            }
            continue;
        }
        if (level === "error" || level === "fatal") {
            faults.push({ kind: "error", line });
        }
    }
    // The deaths the log ended before telling their ends.
    for (const k of new Set([...dying.keys(), ...named.keys()])) {
        unended(k);
    }
    for (const failed of lost.values()) {
        faults.push(...failed.map((line): LogFault => ({ kind: "error", line })));
    }
    return faults;
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
                line.includes("[warning]") ||
                line.includes("[error]") ||
                line.includes("[fatal]") ||
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
