/// Standalone snap suite: thin vitest glue over the driver library in
/// tools/snap.ts. No server involved — each fixture spawns one
/// `clice inspect`; test.concurrent lets vitest own the parallelism
/// (maxConcurrency in vitest.snap.config.ts).

import { beforeAll, describe, expect, test } from "vitest";
import { generateSnapCDBs } from "@clice/tools/compile-commands";
import { checkSnapFixture, orphanSnapshots, snapCorpora } from "@clice/tools/snap";
import { cliceExecutable } from "./fixtures.ts";

beforeAll(() => {
    generateSnapCDBs();
});

for (const corpus of snapCorpora()) {
    describe(`snap/${corpus.feature}`, () => {
        for (const fixture of corpus.fixtures) {
            test.skipIf(!fixture.active).concurrent(
                `${corpus.feature}/${fixture.rel}`,
                async () => {
                    // checkSnapFixture throws on any failure, including a
                    // snapshot mismatch.
                    await expect(
                        checkSnapFixture(cliceExecutable(), corpus, fixture),
                    ).resolves.toBeUndefined();
                },
            );
        }

        test("no orphan snapshots", () => {
            expect(orphanSnapshots(corpus)).toEqual([]);
        });
    });
}
