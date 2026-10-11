/// Every case under tests-next/: each registered test a vitest test, and once
/// they all ran, the case's output against its output.snap.
/// CLICE_CASES=<path prefix> runs the cases under it, CLICE_CASES_SHARD=i/n
/// one of n shards.

import { afterAll, describe, test } from "vitest";
import { loadCases, shard } from "@clice/tools/testing/cases/case";
import { runRegistered } from "@clice/tools/testing/cases/context";
import { assemble, checkOutput, checkSize, sections } from "@clice/tools/testing/cases/output";
import { CASES_DIR } from "@clice/tools/testing/cases/project";
import { loadScript, type Registered } from "@clice/tools/testing/cases/script";

const prefix = process.env["CLICE_CASES"] ?? "";
const under = loadCases(CASES_DIR).filter((c) => c.id.startsWith(prefix));
const spec = process.env["CLICE_CASES_SHARD"];
const cases = spec === undefined ? under : shard(under, spec);

/// Why the test has no meaning here, or undefined where it runs.
function except(registered: Registered): string | undefined {
    return registered.options.except?.[process.platform];
}

function nameOf(registered: Registered): string {
    return registered.name === "" ? registered.kind : registered.name;
}

const notApplicable: string[] = [];

for (const c of cases) {
    const registered = await loadScript(c.script);
    const ran: Registered[] = [];
    for (const entry of registered) {
        const why = except(entry);
        if (why === undefined) {
            ran.push(entry);
        } else {
            notApplicable.push(`${c.id} > ${nameOf(entry)}: ${why}`);
        }
    }
    // A suite with no test fails in vitest.
    if (ran.length === 0) {
        continue;
    }
    describe(c.id, () => {
        const results = new Map<string, string[]>();
        for (const entry of ran) {
            test.concurrent(nameOf(entry), async () => {
                const lines = await runRegistered(c, entry);
                checkSize(c, entry.name, lines.length, entry.options.long);
                results.set(entry.name, lines);
            });
        }
        afterAll(() => {
            if (ran.some((entry) => !results.has(entry.name))) {
                return;
            }
            // A test that has no meaning here keeps the section it left
            // where it runs.
            const kept = sections(c);
            checkOutput(
                c,
                assemble(
                    registered.map((entry) => ({
                        name: entry.name,
                        lines: results.get(entry.name) ?? kept.get(entry.name) ?? [],
                    })),
                ),
            );
        });
    });
}

afterAll(() => {
    if (notApplicable.length > 0) {
        console.log(`not applicable on ${process.platform}:\n  ${notApplicable.join("\n  ")}`);
    }
});
