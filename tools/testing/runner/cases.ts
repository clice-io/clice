/// Every case under tests-next/: each registered test a vitest test, and once
/// they all ran, the case's output against its snap.toml, and a documented
/// case's card under CLICE_CARDS_DIR. CLICE_CASES=<path prefix> runs the
/// cases under it, CLICE_CASES_SHARD=i/n one of n shards.

import { afterAll, describe, test } from "vitest";
import { loadCases, shard } from "@clice/tools/testing/cases/case";
import { runRegistered } from "@clice/tools/testing/cases/context";
import { checkOutput, recorded, type Blocks } from "@clice/tools/testing/cases/output";
import { CASES_DIR } from "@clice/tools/testing/cases/project";
import { loadScript, type Registered } from "@clice/tools/testing/cases/script";
import { writeCard } from "@clice/tools/testing/docs/cards";

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
        const results = new Map<string, Blocks>();
        for (const entry of ran) {
            test.concurrent(nameOf(entry), async () => {
                results.set(nameOf(entry), await runRegistered(c, entry));
            });
        }
        afterAll(() => {
            if (ran.some((entry) => !results.has(nameOf(entry)))) {
                return;
            }
            // A test that has no meaning here keeps the blocks it left
            // where it runs.
            const kept = recorded(c);
            const snapshot = new Map(
                registered.map((entry) => {
                    const name = nameOf(entry);
                    return [name, results.get(name) ?? kept.get(name) ?? new Map()];
                }),
            );
            writeCard(c, checkOutput(c, snapshot));
        });
    });
}

afterAll(() => {
    if (notApplicable.length > 0) {
        console.log(`not applicable on ${process.platform}:\n  ${notApplicable.join("\n  ")}`);
    }
});
