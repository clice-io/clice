/// Before the cases run, the toolchain query they make first, waited out:
/// a compiler's first start on a fresh Windows runner, scanned by its
/// antivirus beside the cases starting, takes past the 10 s clice gives a
/// query, and those cases would show the stopped query. The query is
/// clice's own for the first case's first unit, its database written as
/// the case's is, so it is the one the cases make, whatever they compile
/// with.

import * as fs from "node:fs";
import * as path from "node:path";
import { ending, runProcess } from "../../client/process.ts";
import { cliceExecutable } from "../../client/session.ts";
import { serverEnv } from "../../process_gate.ts";
import { selectedCases } from "./case.ts";
import { copyRoot, materialize, removeCopy, writeDatabases } from "./project.ts";

/// Hang detection: a cold start takes seconds.
const QUERY_MS = 120_000;

export default async function setup(): Promise<void> {
    for (const c of selectedCases()) {
        const root = copyRoot("clice-warm-");
        try {
            writeDatabases(c.project, materialize(c.dir, root), root);
            const database = path.join(root, "compile_commands.json");
            const [first] = fs.existsSync(database)
                ? (JSON.parse(fs.readFileSync(database, "utf8")) as { file: string }[])
                : [];
            if (first === undefined) {
                continue;
            }
            const started = Date.now();
            const result = await runProcess(cliceExecutable(), ["command", first.file], {
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
                    `the warm-up's clice command ${first.file} over ${c.id} asked no compiler ` +
                        `(${ending(result)}), so it warms none up:\n${result.stderr}`,
                );
            }
            console.log(`warm-up: ${asked[0]}, for ${c.id}, in ${Date.now() - started} ms`);
            return;
        } finally {
            await removeCopy(root);
        }
    }
}
