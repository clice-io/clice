/// Before the cases run, each compiler they ask about started once: a
/// compiler's first start on a fresh Windows runner, scanned by its
/// antivirus beside the cases starting, takes past the 10 s clice gives a
/// query, and the cases asking it first would show the stopped query. The
/// compilers are the ones the selected cases' databases run by name: the
/// generated databases' and those a case's `compile_commands.json.in`
/// lists; one a case runs by a path is its own, which no other case starts.
/// Each is asked through clice's own query, for an empty unit alone, so a
/// case's input broken on purpose stops no run.

import * as fs from "node:fs";
import * as path from "node:path";
import { ending, runProcess } from "../../client/process.ts";
import { cliceExecutable } from "../../client/session.ts";
import { serverEnv } from "../../process_gate.ts";
import { selectedCases, type Case } from "./case.ts";
import {
    SOURCE,
    copyName,
    copyRoot,
    posix,
    projectUnits,
    removeCopies,
    unitCompiler,
} from "./project.ts";

/// Hang detection: a cold start takes seconds.
const QUERY_MS = 120_000;

/// A compiler to start, with the name of a source a case compiles with it.
interface Wanted {
    unit: string;
    by: string;
}

/// The compiler and the source of a database entry the case lists.
function listed(entry: unknown): [string, string] | undefined {
    if (typeof entry !== "object" || entry === null) {
        return undefined;
    }
    const { arguments: args, command, file } = entry as Record<string, unknown>;
    const compiler: unknown = Array.isArray(args)
        ? (args as unknown[])[0]
        : typeof command === "string"
          ? command.trim().split(/\s+/)[0]
          : undefined;
    return typeof compiler === "string" && typeof file === "string"
        ? [compiler, path.posix.basename(file)]
        : undefined;
}

/// The compilers the case's databases run by name, each with a source one
/// of them compiles.
function compilers(c: Case, wanted: Map<string, Wanted>): void {
    const want = (compiler: string, unit: string) => {
        if (!/[\\/]/.test(compiler) && SOURCE.test(unit) && !wanted.has(compiler)) {
            wanted.set(compiler, { unit, by: c.id });
        }
    };
    for (const unit of Object.keys(projectUnits(c.project, c.inputs.map(copyName)))) {
        want(unitCompiler(unit), path.posix.basename(unit));
    }
    for (const input of c.inputs) {
        if (path.posix.basename(input) !== "compile_commands.json.in") {
            continue;
        }
        let entries: unknown;
        try {
            entries = JSON.parse(fs.readFileSync(path.join(c.dir, input), "utf8"));
        } catch {
            // A database broken on purpose names no compiler.
            continue;
        }
        for (const entry of Array.isArray(entries) ? entries : []) {
            const found = listed(entry);
            if (found !== undefined) {
                want(...found);
            }
        }
    }
}

export default async function setup(): Promise<void> {
    const wanted = new Map<string, Wanted>();
    for (const c of selectedCases()) {
        compilers(c, wanted);
    }
    for (const [compiler, { unit, by }] of wanted) {
        const root = copyRoot("clice-warm-");
        const file = posix(path.join(root, unit));
        fs.writeFileSync(file, "");
        const entry = {
            directory: posix(root),
            file,
            arguments: [compiler, "-fsyntax-only", file],
        };
        fs.writeFileSync(path.join(root, "compile_commands.json"), JSON.stringify([entry]));
        const started = Date.now();
        const result = await runProcess(cliceExecutable(), ["command", unit], {
            cwd: root,
            env: {
                ...serverEnv(),
                CLICE_LOG_LEVEL: "info",
                CLICE_TEST_QUERY_BOUND_MS: String(QUERY_MS),
            },
            timeout: QUERY_MS,
        });
        const took = Date.now() - started;
        const [unremoved] = await removeCopies(root);
        if (unremoved !== undefined) {
            throw unremoved;
        }
        const asked = /Queried \d+ compilers? for default settings.*/.exec(result.stderr);
        if (asked === null) {
            throw new Error(
                `the warm-up's clice command ${unit} under ${compiler}, of ${by}, asked no ` +
                    `compiler (${ending(result)}), so it warms none up:\n${result.stderr}`,
            );
        }
        console.log(`warm-up: ${compiler}: ${asked[0]}, for ${by}, in ${took} ms`);
    }
}
