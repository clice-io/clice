/// Whether the compiler accepts a unit clice reported errors on: its
/// database command run again, checking the code only. A unit the compiler
/// rejects too is the project's or the setup's, not clice's.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import type { DatabaseEntry } from "../compat/database.ts";
import { runProcess } from "../client/process.ts";
import { syntaxOnly } from "./database.ts";

export interface Verdict {
    unit: string;
    accepted: boolean;
    /// The compiler's first lines, when it rejects the unit.
    output: string;
}

/// Hang detection: a compile of one unit, the slowest a few minutes.
const COMPILE_MS = 20 * 60_000;

export async function confirm(
    entries: readonly DatabaseEntry[],
    units: readonly string[],
): Promise<Verdict[]> {
    // By the file's real path: Bazel names a source through its execution
    // root, whose entries link back into the workspace clice names it by.
    const real = (file: string) => (fs.existsSync(file) ? fs.realpathSync(file) : file);
    const byUnit = new Map<string, DatabaseEntry>();
    for (const entry of entries) {
        const source = real(path.resolve(entry.directory, entry.file));
        if (!byUnit.has(source)) {
            byUnit.set(source, entry);
        }
    }
    const queue = [...units];
    const verdicts: Verdict[] = [];
    const worker = async () => {
        for (let unit = queue.shift(); unit !== undefined; unit = queue.shift()) {
            const entry = byUnit.get(real(unit));
            if (entry === undefined) {
                verdicts.push({ unit, accepted: false, output: "no database entry names it" });
                continue;
            }
            const [compiler, ...args] = syntaxOnly(entry);
            const result = await runProcess(compiler ?? "", args, {
                cwd: entry.directory,
                timeout: COMPILE_MS,
            });
            verdicts.push({
                unit,
                accepted: result.status === 0,
                output:
                    result.status === 0
                        ? ""
                        : (result.error?.message ?? result.stderr)
                              .split("\n")
                              .slice(0, 12)
                              .join("\n"),
            });
        }
    };
    await Promise.all(Array.from({ length: os.availableParallelism() }, worker));
    return verdicts.sort((a, b) => (a.unit < b.unit ? -1 : 1));
}
