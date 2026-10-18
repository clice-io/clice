/// Running clice's commands in a case's copy and the transcript they
/// leave, normalized so it reads the same on every host: the copy is
/// `${WS}`, a session's log directory `${SESSION}`, durations and sizes are
/// elided.

import { execFileSync, spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { runProcess } from "../../client/process.ts";
import { cliceExecutable } from "../../client/session.ts";
import { serverEnv } from "../../process_gate.ts";
import { recordInvocation, recordToml } from "../coverage/record.ts";
import { renderDiff } from "../render/edits.ts";
import { elideCrash } from "../render/lsp.ts";
import { resourceRoot } from "../render/paths.ts";
import type { Table } from "../render/value.ts";

const LOG_LINE = /^\[[^\]]+\] \[(\w+)\] \[thread \d+\] \[[^\]]+\] (.*)$/;

function posixOf(rel: string): string {
    return rel.split(path.sep).join("/");
}

function quote(word: string): string {
    return /^[\w./:=@%+,-]+$/.test(word) ? word : `"${word.replaceAll('"', '\\"')}"`;
}

/// Every spelling of the workspace root the output may use: as given,
/// resolved, with forward slashes, each with a lowercase drive.
function rootForms(root: string): string[] {
    const forms = new Set<string>();
    for (const form of [root, fs.realpathSync.native(root)]) {
        for (const spelled of [form, form.split(path.sep).join("/")]) {
            forms.add(spelled);
            forms.add(spelled.replace(/^[A-Z]:/, (drive) => drive.toLowerCase()));
        }
        forms.add(JSON.stringify(form).slice(1, -1));
    }
    return [...forms].sort((a, b) => b.length - a.length);
}

/// The copy's files and directories in their native spelling, longest
/// first: on Windows a path under the root may hold spaces, which no
/// pattern over the text can tell from the words around it.
function nativePaths(root: string): string[] {
    if (path.sep === "/") {
        return [];
    }
    return fs
        .readdirSync(root, { recursive: true, encoding: "utf8" })
        .filter((rel) => rel.includes(path.sep))
        .sort((a, b) => b.length - a.length);
}

/// `text` as it reads on every host: the copy `${WS}` with forward slashes,
/// a worker's cause of death `${CRASH}`.
export function portable(text: string, root: string): string {
    let out = text;
    const forms = rootForms(root);
    // Most texts name no path under the root, and the walk of its tree is
    // for those that do: over a whole project it takes seconds a text.
    if (forms.some((form) => out.includes(form))) {
        const rels = nativePaths(root);
        for (const form of forms) {
            const sep = form.includes("\\\\") ? "\\\\" : path.sep;
            for (const rel of rels) {
                out = out.replaceAll(
                    `${form}${sep}${rel.split(path.sep).join(sep)}`,
                    `\${WS}/${posixOf(rel)}`,
                );
            }
            out = out.replaceAll(form, "${WS}");
        }
    }
    return elideCrash(out).replace(/(\$\{WS\}[^\s"']*)/g, (span) =>
        span.replaceAll("\\\\", "/").replaceAll("\\", "/"),
    );
}

/// What a command printed with clang's resource directory beside the clice
/// that ran it read `${RESOURCE}`, in every spelling the output may use.
function resourceless(text: string): string {
    let out = text;
    for (const dir of resourceRoot().dirs) {
        for (const form of rootForms(dir)) {
            out = out.replaceAll(form, "${RESOURCE}");
        }
    }
    return out;
}

/// A command's output as it reads on every host and every run: portable, a
/// session's log directory `${SESSION}`, times of day, durations and sizes
/// elided.
export function normalize(text: string, root: string): string {
    return portable(text, root)
        .replace(/\b\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_\d+\b/g, "${SESSION}")
        .replace(/\b\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\b/g, "#time")
        .replace(/\b\d+(\.\d+)?(ms|s)\b/g, "#$2")
        .replace(/\b\d+(\.\d+)? (B|KB|MB|GB)\b/g, "# $2")
        .replace(/\(pid \d+\)/g, "(pid #)");
}

let version: string | undefined;

/// The version the build under test reports, which every commit changes:
/// a command's output reads it `${VERSION}`.
export function buildVersion(): string {
    version ??= /^clice version (\S+)/m.exec(
        execFileSync(cliceExecutable(), ["--version"], { encoding: "utf8" }),
    )?.[1];
    if (version === undefined) {
        throw new Error("clice --version names no version");
    }
    return version;
}

/// Warnings and errors from stderr, the log decoration dropped; plain lines
/// (a message the command prints itself) stay, progress lines go.
function stderrText(stderr: string): string {
    const out: string[] = [];
    // A Windows log line ends in \r\r\n: the logger's \r\n through a
    // text-mode stream.
    for (const line of stderr.split(/\r*\n/)) {
        if (line.trim() === "" || line.startsWith("progress ")) {
            continue;
        }
        const log = LOG_LINE.exec(line);
        if (log === null) {
            out.push(line.trimEnd());
        } else if (log[1] === "warning" || log[1] === "error" || log[1] === "critical") {
            out.push(`${log[1]}: ${log[2] ?? ""}`);
        }
    }
    return out.join("\n");
}

/// Every string of a JSON value, keys included, through `map`.
function mapStrings(value: unknown, map: (text: string) => string): unknown {
    if (typeof value === "string") {
        return map(value);
    }
    if (Array.isArray(value)) {
        return value.map((item) => mapStrings(item, map));
    }
    if (typeof value === "object" && value !== null) {
        return Object.fromEntries(
            Object.entries(value).map(([key, item]) => [map(key), mapStrings(item, map)]),
        );
    }
    return value;
}

/// A key whose number is a duration or a size, which every run reads
/// differently: `seconds`, `mergeMs`, `shardBytes`.
const MEASURE = /^seconds$|(Ms|Bytes)$/;

/// The numbers of a JSON value under a duration's or a size's key elided
/// to `#`, as the text's are.
function elideMeasures(value: unknown): unknown {
    if (Array.isArray(value)) {
        return value.map(elideMeasures);
    }
    if (typeof value === "object" && value !== null) {
        return Object.fromEntries(
            Object.entries(value).map(([key, item]) => [
                key,
                typeof item === "number" && MEASURE.test(key) ? "#" : elideMeasures(item),
            ]),
        );
    }
    return value;
}

/// stdout normalized, JSON pretty-printed: its strings are normalized as
/// parsed, so an escape is never read as part of a path.
export function stdoutText(stdout: string, root: string): string {
    const trimmed = stdout.trimEnd();
    let json: unknown;
    try {
        json = JSON.parse(trimmed);
    } catch {
        return normalize(trimmed, root)
            .split("\n")
            .map((line) => line.trimEnd())
            .join("\n");
    }
    return JSON.stringify(
        elideMeasures(mapStrings(json, (text) => normalize(text, root))),
        null,
        2,
    );
}

/// Split a command line as a POSIX shell does for plain words and double
/// or single quotes; nothing else is shell syntax here.
export function splitCommand(line: string): string[] {
    const words: string[] = [];
    let word = "";
    let quote: string | null = null;
    let started = false;
    for (const char of line) {
        if (quote !== null) {
            if (char === quote) {
                quote = null;
            } else {
                word += char;
            }
        } else if (char === '"' || char === "'") {
            quote = char;
            started = true;
        } else if (char === " ") {
            if (started) {
                words.push(word);
            }
            word = "";
            started = false;
        } else {
            word += char;
            started = true;
        }
    }
    if (quote !== null) {
        throw new Error(`unterminated quote in: ${line}`);
    }
    if (started) {
        words.push(word);
    }
    return words;
}

export interface CommandRun {
    /// `$ clice …`, the transcript block's label.
    label: string;
    /// stdout, stderr at warning and above, the exit, the files written.
    transcript: Table;
    status: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
    /// stdout parsed, when it is JSON.
    json: unknown;
}

export interface CommandOptions {
    env?: Record<string, string>;
    /// The working directory, relative to the copy; its root by default.
    cwd?: string;
    /// Signal the run once it parked after `units` indexed units
    /// (CLICE_TEST_PAUSE): each signal in turn, at once.
    interrupt?: { units: number; signals: NodeJS.Signals[] };
    /// A worker of the run crashes on every such request of the file
    /// (CLICE_TEST_CRASH_REQUEST), as a server's `crash` does; the test's
    /// gate then expects that crash in the command's log. Set by `t.cli`.
    crash?: { request: string; file: string };
    /// Where stdout goes instead of a pipe read to its end: a pipe whose
    /// reader leaves after the first chunk; a FIFO whose reader left before
    /// the run (POSIX); a nonblocking FIFO nobody reads (Linux); a
    /// descriptor open only for reading; or the pipe with stderr joined into
    /// it, as `2>&1` does (POSIX). The transcript then shows no stdout;
    /// with `merged` the run's `stdout` holds what arrived.
    stdout?: StdoutMode;
    /// What the run reads on stdin, as a list piped to `--spec -` is;
    /// nothing when unset.
    input?: string;
}

export type StdoutMode = "closed early" | "fifo gone" | "fifo unread" | "read only" | "merged";

/// Run clice with its stdout where `mode` puts it, until it exits.
function spawnRedirected(
    args: readonly string[],
    cwd: string,
    env: NodeJS.ProcessEnv,
    mode: StdoutMode,
): Promise<{
    status: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
}> {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-stdout-"));
    const fifo = path.join(dir, "out.fifo");
    const opened: number[] = [];
    const open = (file: string, flags: number): number => {
        const fd = fs.openSync(file, flags);
        opened.push(fd);
        return fd;
    };
    // Node makes a child's descriptors 0-2 blocking: a nonblocking one goes
    // in as fd 3 for the shell to put in place.
    const viaShell = (redirect: string): [string, string[]] => [
        "sh",
        ["-c", `exec "$0" "$@" ${redirect}`, cliceExecutable(), ...args],
    ];
    let command: [string, string[]] = [cliceExecutable(), [...args]];
    let stdio: (string | number)[] = ["ignore", "pipe", "pipe"];
    if (mode === "fifo gone" || mode === "fifo unread") {
        execFileSync("mkfifo", [fifo]);
        const reader = open(fifo, fs.constants.O_RDONLY | fs.constants.O_NONBLOCK);
        if (mode === "fifo gone") {
            stdio = ["ignore", open(fifo, fs.constants.O_WRONLY), "pipe"];
            fs.closeSync(reader);
            opened.splice(opened.indexOf(reader), 1);
        } else {
            const writer = open(fifo, fs.constants.O_WRONLY | fs.constants.O_NONBLOCK);
            command = viaShell(">&3");
            stdio = ["ignore", "ignore", "pipe", writer];
        }
    } else if (mode === "read only") {
        const file = path.join(dir, "input");
        fs.writeFileSync(file, "");
        stdio = ["ignore", open(file, fs.constants.O_RDONLY), "pipe"];
    } else if (mode === "merged") {
        command = viaShell("2>&1");
    }
    return new Promise((resolve, reject) => {
        const child = spawn(command[0], command[1], {
            cwd,
            env,
            timeout: 300_000,
            stdio: stdio as ("ignore" | "pipe" | number)[],
        });
        let stdout = "";
        let stderr = "";
        child.stdout?.setEncoding("utf8").on("data", (chunk: string) => {
            stdout += chunk;
            if (mode === "closed early") {
                child.stdout?.destroy();
            }
        });
        child.stderr?.setEncoding("utf8").on("data", (chunk: string) => (stderr += chunk));
        // The child holds its own copies: the writer's last one must be
        // its, so that a reader sees the end.
        if (mode !== "fifo unread") {
            for (const fd of opened.splice(0)) {
                fs.closeSync(fd);
            }
        }
        child.on("error", reject);
        child.on("close", (status, signal) => {
            for (const fd of opened) {
                fs.closeSync(fd);
            }
            fs.rmSync(dir, { recursive: true, force: true });
            resolve({ status, signal, stdout, stderr });
        });
    });
}

/// Run clice until it exits; with `interrupt`, signal it once the pause
/// point it was given is reached.
function spawnClice(
    args: readonly string[],
    cwd: string,
    env: NodeJS.ProcessEnv,
    interrupt: CommandOptions["interrupt"],
    input?: string,
): Promise<{
    status: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
}> {
    if (interrupt === undefined) {
        return runProcess(cliceExecutable(), args, {
            cwd,
            env,
            timeout: 300_000,
            ...(input === undefined ? {} : { input }),
        });
    }
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-pause-"));
    const marker = path.join(dir, "paused");
    return new Promise((resolve, reject) => {
        const child = spawn(cliceExecutable(), args, {
            cwd,
            env: { ...env, CLICE_TEST_PAUSE: `${String(interrupt.units)} ${marker}` },
            timeout: 300_000,
        });
        let stdout = "";
        let stderr = "";
        child.stdout.setEncoding("utf8").on("data", (chunk: string) => (stdout += chunk));
        child.stderr.setEncoding("utf8").on("data", (chunk: string) => (stderr += chunk));
        child.stdin.end();
        let signalled = false;
        // No event: fs.watch loses it on macOS, where one FSEvents stream
        // serves every directory watch of the process and is recreated
        // whenever another test opens or closes one.
        const poll = setInterval(() => {
            if (!fs.existsSync(marker)) {
                return;
            }
            signalled = true;
            clearInterval(poll);
            for (const name of interrupt.signals) {
                child.kill(name);
            }
        }, 10);
        child.on("error", reject);
        child.on("close", (status, sig) => {
            clearInterval(poll);
            fs.rmSync(dir, { recursive: true, force: true });
            if (!signalled) {
                const how = sig === null ? `exited ${String(status)}` : `was killed by ${sig}`;
                reject(new Error(`clice ${args.join(" ")} ${how} before its pause point`));
                return;
            }
            resolve({ status, signal: sig, stdout, stderr });
        });
    });
}

/// How a run ended: its exit status, or the signal that killed it. Windows
/// has no SIGPIPE: there clice ends with 141 where a reader that left would
/// kill it (driver::write_output), the status a POSIX shell reports for
/// that death, and it reads as the signal.
function ending(status: number | null, signal: NodeJS.Signals | null): Table {
    if (status === null) {
        return { killed: String(signal) };
    }
    return process.platform === "win32" && status === 141
        ? { killed: "SIGPIPE" }
        : { exit: status };
}

/// Every file under `root` but the caches, by relative path.
function contents(root: string): Map<string, Buffer> {
    const out = new Map<string, Buffer>();
    for (const rel of fs.readdirSync(root, { recursive: true, encoding: "utf8" })) {
        const file = path.join(root, rel);
        const name = rel.split(path.sep).join("/");
        if (
            !name.split("/").includes(".clice") &&
            fs.statSync(file, { throwIfNoEntry: false })?.isFile() === true
        ) {
            out.set(name, fs.readFileSync(file));
        }
    }
    return out;
}

/// The files a command changed, each as a diff.
function written(before: Map<string, Buffer>, after: Map<string, Buffer>): Table {
    const out: Record<string, string> = {};
    for (const name of [...new Set([...before.keys(), ...after.keys()])].sort()) {
        const old = before.get(name);
        const now = after.get(name);
        if (old !== undefined && now !== undefined && old.equals(now)) {
            continue;
        }
        // A file created empty has no line a diff could show.
        out[name] =
            now === undefined
                ? "(removed)"
                : old === undefined && now.length === 0
                  ? "(created empty)"
                  : renderDiff(old?.toString("utf8") ?? "", now.toString("utf8"));
    }
    return out;
}

/// Run clice in the copy at `root`; the transcript block and the result.
export async function runCommand(
    root: string,
    args: readonly string[],
    options: CommandOptions = {},
): Promise<CommandRun> {
    if (fs.existsSync(path.join(root, "clice.toml"))) {
        recordToml(fs.readFileSync(path.join(root, "clice.toml"), "utf8"));
    }
    recordInvocation(args);
    const before = contents(root);
    const cwd = path.join(root, options.cwd ?? ".");
    const env = { ...serverEnv(), ...options.env };
    const result =
        options.stdout === undefined
            ? await spawnClice(args, cwd, env, options.interrupt, options.input)
            : await spawnRedirected(args, cwd, env, options.stdout);
    let json: unknown;
    try {
        json = JSON.parse(result.stdout);
    } catch {
        // Not JSON.
    }
    const versioned = (text: string): string => text.replaceAll(buildVersion(), "${VERSION}");
    const stdout = stdoutText(resourceless(versioned(result.stdout)), root);
    const stderr = stderrText(normalize(resourceless(versioned(result.stderr)), root));
    const files = written(before, contents(root));
    return {
        label: [
            "$ clice",
            ...args.map((arg) => quote(normalize(arg, root))),
            ...(options.stdout === undefined ? [] : [`(stdout: ${options.stdout})`]),
        ].join(" "),
        transcript: {
            ...(stdout === "" || options.stdout !== undefined ? {} : { stdout }),
            ...(stderr === "" ? {} : { stderr }),
            ...ending(result.status, result.signal),
            ...(Object.keys(files).length === 0 ? {} : { files }),
        },
        status: result.status,
        signal: result.signal,
        stdout: result.stdout,
        stderr: result.stderr,
        json,
    };
}
