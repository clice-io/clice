/// The snap suite: thin vitest glue over the snap domain in tools/snap/.
/// Each corpus is pinned from both paths — standalone (`clice inspect`,
/// one concurrent process per fixture, no server) and wire (replayed
/// through a real server) — plus the not-yet-migrated legacy corpora,
/// wire-only. The integration suite plays no part in snapshots.

import { beforeAll, describe } from "vitest";
import { generateTestDataCDBs } from "@clice/tools/compile-commands";
import {
    checkSnapFixture,
    generateSnapCDBs,
    orphanSnapshots,
    snapCorpora,
} from "@clice/tools/snap/standalone";
import {
    checkLegacyWireFixture,
    checkWireSnapFixture,
    legacyCorpora,
    wirePresenter,
} from "@clice/tools/snap/wire";
import { cliceExecutable, expect, test } from "./fixtures.ts";

beforeAll(() => {
    // Corpus CDBs for the standalone/wire drivers, plus the legacy
    // tests/data corpora the wire-only path still replays.
    generateSnapCDBs();
    generateTestDataCDBs();
});

for (const corpus of snapCorpora()) {
    wirePresenter(corpus.feature); // both drivers must cover every corpus
    describe(`snap/${corpus.feature}`, () => {
        // All standalone cases register contiguously: vitest closes a
        // concurrent batch at the first sequential test, so interleaving
        // them with the wire cases would serialize the inspect processes.
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

        for (const fixture of corpus.fixtures) {
            test.skipIf(!fixture.active)(
                `${corpus.feature}/${fixture.rel} (wire)`,
                async ({ session }) => {
                    await checkWireSnapFixture(
                        await session(`snap/${corpus.feature}`),
                        corpus,
                        fixture,
                    );
                },
            );
        }

        test("no orphan snapshots", () => {
            expect(orphanSnapshots(corpus)).toEqual([]);
        });
    });
}

for (const corpus of legacyCorpora()) {
    describe(`snap/${corpus.feature} (legacy wire)`, () => {
        for (const rel of corpus.fixtures) {
            test(`${corpus.feature}/${rel}`, async ({ session }) => {
                await checkLegacyWireFixture(() => session(corpus.feature), corpus, rel);
            });
        }
    });
}
