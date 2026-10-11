/// One-off check of the switch_source_header snap corpus migration (S-NAV):
/// every counterpart an old snapshot pinned for a file, its reasons and the
/// preferred one, is in the snap.toml of the case its fixture moved to, and
/// the new answer holds nothing the old one did not. A dropped fixture
/// names the case pinning the same. Run it from the repository:
/// `node tools/migrate/switch_source_header.ts`.

import type { Table } from "../testing/render/value.ts";
import { corpus, newBlocks, oldBlocks, OldFiles } from "./s_nav_shared.ts";

/// Fixture → case and the test its files moved to; `drop` names the case
/// pinning the same.
const PLAN: Record<string, { to: string; test: string; drop?: string }> = {
    "switch_source_header/01_header_source/main.cpp": {
        to: "counterparts/01_header_and_source",
        test: "answers",
        drop: "the same pair: a header and the source of its name defining all it declares, both ways",
    },
    "switch_source_header/02_split_layout/main.cpp": {
        to: "layouts/01_split_directories",
        test: "answers",
    },
    "switch_source_header/03_scattered_definitions/main.cpp": {
        to: "ranking/01_scattered_definitions",
        test: "answers",
    },
    "switch_source_header/04_split_evenly/main.cpp": {
        to: "ranking/02_split_evenly",
        test: "answers",
    },
    "switch_source_header/05_module_units/main.cpp": {
        to: "modules/01_module_units",
        test: "answers",
    },
    "switch_source_header/06_module_partition/main.cpp": {
        to: "modules/02_partitions",
        test: "answers",
    },
    "switch_source_header/07_no_counterpart/main.cpp": {
        to: "counterparts/03_no_counterpart",
        test: "answers",
    },
    "switch_source_header/08_template_definitions/main.cpp": {
        to: "counterparts/02_template_definitions",
        test: "indexed",
    },
};

function main(): void {
    const c = corpus("switch_source_header");
    let asked = 0;
    let unexplained = 0;
    for (const fixture of c.fixtures) {
        const step = PLAN[fixture.rel];
        if (step === undefined) {
            throw new Error(`unplanned fixture ${fixture.rel}`);
        }
        const blocks = oldBlocks(c, fixture);
        asked += blocks.length;
        if (step.drop !== undefined) {
            console.log(
                `dropped ${fixture.rel} (${String(blocks.length)} files): ${step.to}, ${step.drop}`,
            );
            continue;
        }
        const files = new OldFiles(c, fixture);
        const tests = newBlocks(`switch_source_header/${step.to}`).get(step.test);
        for (const block of blocks) {
            const answer = tests?.get(block.name)?.["counterparts"] as Table | undefined;
            const old = JSON.stringify({
                ...(block.values["preferred"] === undefined
                    ? {}
                    : { preferred: files.newPath(block.values["preferred"]) }),
                candidates: block.entries.map(({ fields }) => ({
                    path: files.newPath(fields["file"] ?? ""),
                    reasons: JSON.parse(fields["reasons"] ?? "[]") as string[],
                })),
            });
            const now = JSON.stringify(answer ?? null);
            if (old !== now) {
                unexplained += 1;
                console.log(
                    `${fixture.rel} → ${step.to} ${block.name}\n  old ${old}\n  new ${now}`,
                );
            }
        }
    }
    console.log(`old: ${String(asked)} files asked`);
    console.log(`unexplained differences: ${String(unexplained)}`);
    process.exitCode = unexplained === 0 ? 0 : 1;
}

main();
