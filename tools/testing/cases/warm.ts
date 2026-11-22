/// Before the cases run, the toolchain query they make first, waited out:
/// a compiler's first start on a fresh Windows runner, scanned by its
/// antivirus beside the cases starting, takes past the 10 s clice gives a
/// query, and those cases would show the stopped query. The query is
/// clice's own for the first case's first unit, its command written as the
/// case's is, so it is the one the cases make, whatever they compile with;
/// the unit is empty and alone, so a case's input broken on purpose stops
/// no run.

import * as fs from "node:fs";
import * as path from "node:path";
import { ending, runProcess } from "../../client/process.ts";
import { cliceExecutable } from "../../client/session.ts";
import { serverEnv } from "../../process_gate.ts";
import { selectedCases } from "./case.ts";
import { copyName, copyRoot, projectUnits, removeCopy, writeDatabase } from "./project.ts";

/// Hang detection: a cold start takes seconds.
const QUERY_MS = 120_000;

export default async function setup(): Promise<void> {
    for (const c of selectedCases()) {
        const [first] = Object.entries(projectUnits(c.project, c.inputs.map(copyName)));
        if (first === undefined) {
            continue;
        }
        const [unit, args] = first;
        const root = copyRoot("clice-warm-");
        try {
            fs.mkdirSync(path.dirname(path.join(root, unit)), { recursive: true });
            fs.writeFileSync(path.join(root, unit), "");
            writeDatabase(root, c.project, { [unit]: args });
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
            const asked = /Queried \d+ compilers? for default settings.*/.exec(result.stderr);
            if (asked === null) {
                throw new Error(
                    `the warm-up's clice command ${unit} of ${c.id} asked no compiler ` +
                        `(${ending(result)}), so it warms none up:\n${result.stderr}`,
                );
            }
            console.log(`warm-up: ${asked[0]}, for ${c.id}, in ${Date.now() - started} ms`);
        } finally {
            await removeCopy(root);
        }
        return;
    }
}
