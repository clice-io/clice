/// Whether the compiler confirms the errors clice reported on a unit: its
/// database command run again, checking the code only. A unit the compiler
/// rejects for the same errors is the project's or the setup's, not
/// clice's.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { crashed, ending, runProcess } from "../../client/process.ts";
import { entrySource, isMsvc, type DatabaseEntry } from "../../compat/database.ts";
import { syntaxOnly } from "../database.ts";

/// An error clice reported: where, when it says (`file` absolute, `line`
/// and `column` from 1), and what.
export interface ReportedError {
    file?: string;
    line?: number;
    column?: number;
    message: string;
}

export interface CompilerCheck {
    /// `rejected`: the compiler rejects the unit for an error clice
    /// reported too; `differs`: for others alone; `unchecked`: the compiler
    /// could not be asked, or did not compile the file, `output` says why.
    outcome: "accepted" | "rejected" | "differs" | "unchecked";
    /// The compiler's first lines, when it does not accept the unit.
    output: string;
}

/// Hang detection: a compile of one unit, the slowest a few minutes.
const COMPILE_MS = 20 * 60_000;

/// The inputs a command reads that only the project's build writes, and
/// that are missing: a response file (CMake's and Bazel's module maps) or a
/// module file.
export function missingInputs(args: readonly string[], directory: string): string[] {
    return args
        .flatMap((arg) => {
            if (arg.startsWith("@")) {
                return [arg.slice(1)];
            }
            const module = /^-fmodule-file=(?:[^=]+=)?(.+)$/.exec(arg)?.[1];
            return module === undefined ? [] : [module];
        })
        .filter((file) => !fs.existsSync(path.resolve(directory, file)));
}

/// The database's entry of a unit, its first. By the file's real path:
/// Bazel names a source through its execution root, whose entries link
/// back into the workspace clice names it by. On Windows clice spells a
/// drive in lowercase and the build in uppercase: the file system's own
/// spelling is the one both resolve to.
export function entryOf(
    entries: readonly DatabaseEntry[],
): (unit: string) => DatabaseEntry | undefined {
    const real = (file: string) => (fs.existsSync(file) ? fs.realpathSync.native(file) : file);
    const byUnit = new Map<string, DatabaseEntry>();
    for (const entry of entries) {
        const source = real(entrySource(entry));
        if (!byUnit.has(source)) {
            byUnit.set(source, entry);
        }
    }
    return (unit) => byUnit.get(real(unit));
}

export interface CompilerError {
    file: string;
    line: number;
    message: string;
}

/// The errors a compiler's output locates in a file: GCC's and clang's
/// `file:line:column: error: message`, cl's and clang-cl's
/// `file(line,column): error C1234: message`. A driver's own errors (an
/// unknown option, a missing input) name no line.
export function compilerErrors(output: string, directory: string): CompilerError[] {
    return output.split(/\r?\n/).flatMap((line) => {
        const found =
            /^(.+?):(\d+):(?:\d+:)? (?:fatal )?error: (.*)$/.exec(line) ??
            /^(.+?)\((\d+)(?:,\d+)?\) ?: (?:fatal )?error(?: [A-Z]+\d+)?: (.*)$/.exec(line);
        if (found === null) {
            return [];
        }
        const [, file = "", at = "", message = ""] = found;
        return [{ file: path.resolve(directory, file.trim()), line: Number(at), message }];
    });
}

/// A file as two tools spelling it differently both reach it.
function sameFile(a: string, b: string): boolean {
    const real = (file: string) => {
        const resolved = fs.existsSync(file) ? fs.realpathSync.native(file) : file;
        return process.platform === "win32" ? resolved.toLowerCase() : resolved;
    };
    return real(a) === real(b);
}

/// A message as two compilers may both word it: quotes and the warning
/// option a message ends with aside.
function wording(message: string): string {
    return message
        .replace(/\s*\[-W[^\]]*\]$/, "")
        .replace(/[‘’`]/g, "'")
        .trim()
        .toLowerCase();
}

/// Whether the compiler's errors confirm clice's: one of them on the line
/// of clice's first, or worded as it. Without an error of clice's (a unit
/// that failed to index), any error of the compiler's does.
export function confirms(
    reported: readonly ReportedError[],
    found: readonly CompilerError[],
): boolean {
    const [first] = reported;
    if (first === undefined) {
        return found.length > 0;
    }
    return found.some(
        (error) =>
            (first.file !== undefined &&
                first.line === error.line &&
                sameFile(first.file, error.file)) ||
            wording(first.message) === wording(error.message),
    );
}

/// Each unit (`item.unit`) with what the compiler says of it against the
/// errors clice reported (`item.errors`); a header with what it says of
/// the unit including it (`item.host`). The compiler runs in `env`, the
/// configuration's environment.
export async function confirm<
    T extends { unit: string; host?: string | undefined; errors?: readonly ReportedError[] },
>(
    entries: readonly DatabaseEntry[],
    items: readonly T[],
    env: NodeJS.ProcessEnv | undefined,
): Promise<(T & CompilerCheck)[]> {
    const find = entryOf(entries);
    const queue = [...items];
    const checks: (T & CompilerCheck)[] = [];
    const check = async (
        unit: string,
        reported: readonly ReportedError[],
    ): Promise<CompilerCheck> => {
        const entry = find(unit);
        if (entry === undefined) {
            return { outcome: "unchecked", output: "no entry of the database names it" };
        }
        const [compiler, ...args] = syntaxOnly(entry);
        const missing = missingInputs(args, entry.directory);
        if (missing.length > 0) {
            return {
                outcome: "unchecked",
                output: `its command reads what only the build writes: ${missing.join(", ")}`,
            };
        }
        const result = await runProcess(compiler ?? "", args, {
            cwd: entry.directory,
            timeout: COMPILE_MS,
            ...(env === undefined ? {} : { env }),
        });
        if (result.error !== undefined || crashed(result)) {
            return { outcome: "unchecked", output: `the compiler ${ending(result)}` };
        }
        // A database command does not say where the build put the modules a
        // unit imports (CMake passes them in a module map at build time).
        const module = /fatal error: module '[^']+' not found/.exec(result.stderr);
        if (module !== null) {
            return {
                outcome: "unchecked",
                output: `the modules it imports are the build's: ${module[0]}`,
            };
        }
        // cl writes its diagnostics to stdout.
        const text = isMsvc(compiler ?? "") ? result.stdout : result.stderr;
        const output = text.split("\n").slice(0, 12).join("\n");
        if (result.status === 0) {
            return { outcome: "accepted", output };
        }
        const found = compilerErrors(text, entry.directory);
        if (found.length === 0) {
            return { outcome: "unchecked", output: `the compiler did not compile it:\n${output}` };
        }
        return { outcome: confirms(reported, found) ? "rejected" : "differs", output };
    };
    const worker = async () => {
        for (let item = queue.shift(); item !== undefined; item = queue.shift()) {
            checks.push({ ...item, ...(await check(item.host ?? item.unit, item.errors ?? [])) });
        }
    };
    await Promise.all(Array.from({ length: os.availableParallelism() }, worker));
    return checks.sort((a, b) => (a.unit < b.unit ? -1 : 1));
}
