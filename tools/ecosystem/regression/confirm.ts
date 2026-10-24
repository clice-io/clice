/// Whether the compiler accepts a unit clice reported errors on: its
/// database command run again, checking the code only. A unit the compiler
/// rejects too is the project's or the setup's, not clice's.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { crashed, ending, runProcess } from "../../client/process.ts";
import { entrySource, isMsvc, type DatabaseEntry } from "../../compat/database.ts";
import { syntaxOnly } from "../database.ts";

export interface CompilerCheck {
    /// `unchecked`: the compiler could not be asked, `output` says why.
    outcome: "accepted" | "rejected" | "unchecked";
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

/// Each unit (`item.unit`) with what the compiler says of it; a header
/// with what it says of the unit including it (`item.host`). The compiler
/// runs in `env`, the configuration's environment.
export async function confirm<T extends { unit: string; host?: string | undefined }>(
    entries: readonly DatabaseEntry[],
    items: readonly T[],
    env: NodeJS.ProcessEnv | undefined,
): Promise<(T & CompilerCheck)[]> {
    // By the file's real path: Bazel names a source through its execution
    // root, whose entries link back into the workspace clice names it by.
    // On Windows clice spells a drive in lowercase and the build in
    // uppercase: the file system's own spelling is the one both resolve to.
    const real = (file: string) => (fs.existsSync(file) ? fs.realpathSync.native(file) : file);
    const byUnit = new Map<string, DatabaseEntry>();
    for (const entry of entries) {
        const source = real(entrySource(entry));
        if (!byUnit.has(source)) {
            byUnit.set(source, entry);
        }
    }
    const queue = [...items];
    const checks: (T & CompilerCheck)[] = [];
    const check = async (unit: string): Promise<CompilerCheck> => {
        const entry = byUnit.get(real(unit));
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
        const output = isMsvc(compiler ?? "") ? result.stdout : result.stderr;
        return {
            outcome: result.status === 0 ? "accepted" : "rejected",
            output: output.split("\n").slice(0, 12).join("\n"),
        };
    };
    const worker = async () => {
        for (let item = queue.shift(); item !== undefined; item = queue.shift()) {
            checks.push({ ...item, ...(await check(item.host ?? item.unit)) });
        }
    };
    await Promise.all(Array.from({ length: os.availableParallelism() }, worker));
    return checks.sort((a, b) => (a.unit < b.unit ? -1 : 1));
}
