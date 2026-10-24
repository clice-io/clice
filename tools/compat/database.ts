/// The compilation database a scenario's build wrote, read the way its
/// own consumers read it.

import * as fs from "node:fs";
import * as path from "node:path";
import { parse } from "shell-quote";
import { run } from "./scenario.ts";

export interface DatabaseEntry {
    directory: string;
    file: string;
    arguments?: string[];
    command?: string;
}

/// The database at the project root or in `build/`, the two places clice
/// looks without configuration.
export function readDatabase(root: string): DatabaseEntry[] {
    const found = ["compile_commands.json", "build/compile_commands.json"]
        .map((rel) => path.join(root, rel))
        .filter((file) => fs.existsSync(file));
    const [only] = found;
    if (found.length !== 1 || only === undefined) {
        throw new Error(`expected one compile_commands.json under ${root}, found ${found.length}`);
    }
    return JSON.parse(fs.readFileSync(only, "utf8")) as DatabaseEntry[];
}

/// A Windows command line split by the CommandLineToArgvW rules the
/// quoting of build tools relies on: whitespace outside quotes separates, 2n backslashes
/// before a quote are n backslashes and the quote toggles quoting, 2n+1
/// are n backslashes and a literal quote, other backslashes are literal.
/// No maintained npm package implements these rules.
function splitWindows(command: string): string[] {
    const args: string[] = [];
    let current = "";
    let started = false;
    let quoted = false;
    for (let i = 0; i < command.length; i += 1) {
        const c = command[i] ?? "";
        if (c === "\\") {
            let count = 0;
            while (command[i + count] === "\\") {
                count += 1;
            }
            if (command[i + count] === '"') {
                current += "\\".repeat(Math.floor(count / 2));
                if (count % 2 === 1) {
                    current += '"';
                    i += count;
                } else {
                    i += count - 1;
                }
            } else {
                current += "\\".repeat(count);
                i += count - 1;
            }
            started = true;
        } else if (c === '"') {
            quoted = !quoted;
            started = true;
        } else if (!quoted && (c === " " || c === "\t")) {
            if (started) {
                args.push(current);
                current = "";
                started = false;
            }
        } else {
            current += c;
            started = true;
        }
    }
    if (started) {
        args.push(current);
    }
    return args;
}

/// An entry's argv: `arguments` as written, or `command` split by the
/// rules of the platform it was written for — on POSIX by shell-quote,
/// the npm ecosystem's standard shell lexer, with variables kept literal
/// as the compiler received them.
export function entryArguments(entry: DatabaseEntry): string[] {
    if (entry.arguments !== undefined) {
        return entry.arguments;
    }
    if (process.platform === "win32") {
        return splitWindows(entry.command ?? "");
    }
    return shellWords(entry.command ?? "");
}

function shellWords(command: string): string[] {
    return parse(command, (name) => `$${name}`).map((token) => {
        if (typeof token === "string") {
            return token;
        }
        if ("op" in token && token.op === "glob") {
            return token.pattern;
        }
        throw new Error(`shell syntax a compile command cannot carry: ${command}`);
    });
}

/// The Windows paths the Cygwin or MSYS2 runtime in `bin` maps `paths` to.
export async function cygpath(bin: string, paths: string[]): Promise<string[]> {
    const mapped = await run(path.join(bin, "cygpath.exe"), ["-w", ...paths], { env: process.env });
    const lines = mapped.stdout.split(/\r?\n/).slice(0, paths.length);
    if (mapped.status !== 0 || lines.length < paths.length) {
        throw new Error(
            `cygpath failed (${mapped.error ?? `exit ${mapped.status}`}): ${mapped.stderr}`,
        );
    }
    return lines;
}

/// A database written inside Cygwin or MSYS2 as the checks read it: its
/// directories and files mapped to Windows paths by the runtime in `bin`,
/// its commands split as sh splits them. The arguments stay as written:
/// the compiler, which reads them, runs inside that runtime too.
export async function nativeEntries(
    entries: DatabaseEntry[],
    bin: string,
): Promise<DatabaseEntry[]> {
    const mapped = await cygpath(
        bin,
        entries.flatMap((entry) => [entry.directory, entry.file]),
    );
    return entries.map((entry, i) => ({
        directory: mapped[2 * i] ?? "",
        file: mapped[2 * i + 1] ?? "",
        arguments: entry.arguments ?? shellWords(entry.command ?? ""),
    }));
}

const OUTPUTS = new Set(["-c", "-MD", "-MMD", "-MP"]);
const OUTPUTS_WITH_VALUE = new Set(["-o", "-MF", "-MT", "-MQ"]);
/// An object file joined to `-o` (not an `-objc*` option), and the
/// dependency file GCC's driver hands its preprocessor.
const JOINED_OUTPUTS = /^(-o(?!bj)|-Wp,-M+D,)/;
/// cl's options carry their values joined.
const MSVC_OUTPUTS = /^[/-](c|FS|showIncludes|Fo.*|Fd.*)$/;

/// Whether a driver takes cl's options: cl and clang-cl.
export function isMsvc(driver: string): boolean {
    return /^(cl|clang-cl)(\.exe)?$/i.test(path.basename(driver));
}

/// An entry's argv, driver first, without the arguments that name the
/// build's outputs or its own dependency files: rerunning the entry must
/// neither overwrite them nor compile.
export function withoutOutputs(args: readonly string[]): string[] {
    const msvc = isMsvc(args[0] ?? "");
    const out: string[] = [];
    for (let i = 0; i < args.length; i += 1) {
        const arg = args[i] ?? "";
        if (!msvc && OUTPUTS_WITH_VALUE.has(arg)) {
            i += 1;
        } else if (
            msvc ? !MSVC_OUTPUTS.test(arg) : !OUTPUTS.has(arg) && !JOINED_OUTPUTS.test(arg)
        ) {
            out.push(arg);
        }
    }
    return out;
}

export function entrySource(entry: DatabaseEntry): string {
    return path.resolve(entry.directory, entry.file);
}

/// Whether two absolute paths name the same file as the platform compares
/// them: Windows build tools spell drive letters either case.
export function samePath(a: string, b: string): boolean {
    return process.platform === "win32" ? a.toLowerCase() === b.toLowerCase() : a === b;
}
