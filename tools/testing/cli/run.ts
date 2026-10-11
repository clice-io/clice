/// Running clice's commands in a case's copy and the transcript they
/// leave, normalized so it reads the same on every host: the copy is
/// `${WS}`, a session's log directory `${SESSION}`, durations and sizes are
/// elided.

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { runProcess } from "../../client/process.ts";
import { cliceExecutable } from "../../client/session.ts";
import { serverEnv } from "../../process_gate.ts";
import { recordInvocation, recordToml } from "../coverage/record.ts";
import { renderDiff } from "../render/edits.ts";
import { elideCrash } from "../render/lsp.ts";

const LOG_LINE = /^\[[^\]]+\] \[(\w+)\] \[thread \d+\] \[[^\]]+\] (.*)$/;

function posixOf(rel: string): string {
    return rel.split(path.sep).join("/");
}

function quote(word: string): string {
    return /^[\w./:=@%+,-]+$/.test(word) ? word : `"${word.replaceAll('"', '\\"')}"`;
}

/// Every spelling of the workspace root the output may use: as given,
/// resolved, with forward slashes, with a lowercase drive.
function rootForms(root: string): string[] {
    const forms = new Set<string>();
    for (const form of [root, fs.realpathSync.native(root)]) {
        const slashed = form.split(path.sep).join("/");
        forms.add(form);
        forms.add(slashed);
        forms.add(slashed.replace(/^[A-Z]:/, (drive) => drive.toLowerCase()));
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
    const rels = nativePaths(root);
    for (const form of rootForms(root)) {
        const sep = form.includes("\\\\") ? "\\\\" : path.sep;
        for (const rel of rels) {
            out = out.replaceAll(
                `${form}${sep}${rel.split(path.sep).join(sep)}`,
                `\${WS}/${posixOf(rel)}`,
            );
        }
        out = out.replaceAll(form, "${WS}");
    }
    return elideCrash(out).replace(/(\$\{WS\}[^\s"']*)/g, (span) =>
        span.replaceAll("\\\\", "/").replaceAll("\\", "/"),
    );
}

/// A command's output as it reads on every host and every run: portable, a
/// session's log directory `${SESSION}`, durations and sizes elided.
export function normalize(text: string, root: string): string {
    return portable(text, root)
        .replace(/\.clice\/logs\/[^/\s"']+/g, ".clice/logs/${SESSION}")
        .replace(/\b\d+(\.\d+)?(ms|s)\b/g, "#$2")
        .replace(/\b\d+(\.\d+)? (B|KB|MB|GB)\b/g, "# $2");
}

/// Warnings and errors from stderr, the log decoration dropped; plain lines
/// (a message the command prints itself) stay, progress lines go.
function stderrLines(stderr: string): string[] {
    const out: string[] = [];
    // A Windows log line ends in \r\r\n: the logger's \r\n through a
    // text-mode stream.
    for (const line of stderr.split(/\r*\n/)) {
        if (line.trim() === "" || line.startsWith("progress ")) {
            continue;
        }
        const log = LOG_LINE.exec(line);
        if (log === null) {
            out.push(`stderr: ${line}`);
        } else if (log[1] === "warning" || log[1] === "error" || log[1] === "critical") {
            out.push(`${log[1]}: ${log[2] ?? ""}`);
        }
    }
    return out;
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

/// stdout normalized, JSON pretty-printed: its strings are normalized as
/// parsed, so an escape is never read as part of a path.
function stdoutLines(stdout: string, root: string): string[] {
    const trimmed = stdout.trimEnd();
    if (trimmed === "") {
        return [];
    }
    let json: unknown;
    try {
        json = JSON.parse(trimmed);
    } catch {
        return normalize(trimmed, root)
            .split("\n")
            .map((line) => line.trimEnd());
    }
    return JSON.stringify(
        mapStrings(json, (text) => normalize(text, root)),
        null,
        2,
    ).split("\n");
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
    /// `$ clice …`, the transcript block's header.
    header: string;
    /// stdout, stderr at warning and above, the files written, the exit.
    body: string[];
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
}

/// Run clice until it exits; with `interrupt`, signal it once the pause
/// point it was given is reached.
function spawnClice(
    args: readonly string[],
    cwd: string,
    env: NodeJS.ProcessEnv,
    interrupt: CommandOptions["interrupt"],
): Promise<{
    status: number | null;
    signal: NodeJS.Signals | null;
    stdout: string;
    stderr: string;
}> {
    if (interrupt === undefined) {
        return runProcess(cliceExecutable(), args, { cwd, env, timeout: 300_000 });
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
        const signal = (): void => {
            if (signalled || !fs.existsSync(marker)) {
                return;
            }
            signalled = true;
            watcher.close();
            for (const name of interrupt.signals) {
                child.kill(name);
            }
        };
        const watcher = fs.watch(dir, signal);
        signal();
        child.on("error", reject);
        child.on("close", (status, sig) => {
            watcher.close();
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

/// The files a command changed, as diffs.
function written(before: Map<string, Buffer>, after: Map<string, Buffer>): string[] {
    const out: string[] = [];
    for (const name of [...new Set([...before.keys(), ...after.keys()])].sort()) {
        const old = before.get(name);
        const now = after.get(name);
        if (old !== undefined && now !== undefined && old.equals(now)) {
            continue;
        }
        if (now === undefined) {
            out.push(`--- ${name}`, "(removed)");
            continue;
        }
        out.push(...renderDiff(name, old?.toString("utf8") ?? "", now.toString("utf8")));
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
    const result = await spawnClice(
        args,
        path.join(root, options.cwd ?? "."),
        { ...serverEnv(), ...options.env },
        options.interrupt,
    );
    let json: unknown;
    try {
        json = JSON.parse(result.stdout);
    } catch {
        // Not JSON.
    }
    return {
        header: ["$ clice", ...args.map((arg) => quote(normalize(arg, root)))].join(" "),
        body: [
            ...stdoutLines(result.stdout, root),
            ...stderrLines(normalize(result.stderr, root)),
            result.signal === null ? `exit ${String(result.status)}` : `killed by ${result.signal}`,
            ...written(before, contents(root)),
        ],
        status: result.status,
        signal: result.signal,
        stdout: result.stdout,
        stderr: result.stderr,
        json,
    };
}
