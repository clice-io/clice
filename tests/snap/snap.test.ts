/// The snap suite: thin vitest glue over the snap domain in tools/snap/.
/// Each fixture is pinned from the paths its `verify:` mode asks for —
/// inspect (`clice inspect`, one concurrent process per fixture, no
/// server) and server (replayed through a real server on a materialized
/// throwaway workspace). The integration suite plays no part in
/// snapshots.

import { describe } from "vitest";
import { orphanSnapshots, snapCorpora } from "@clice/tools/snap/corpus";
import { checkInspectFixture } from "@clice/tools/snap/inspect";
import { feature } from "@clice/tools/snap/registry";
import { checkServerSnapFixture } from "@clice/tools/snap/server";
import { cliceExecutable, expect, test } from "./fixtures.ts";

for (const corpus of snapCorpora()) {
    feature(corpus.feature); // every corpus must be registered
    describe(`snap/${corpus.feature}`, () => {
        // All inspect cases register contiguously: vitest closes a
        // concurrent batch at the first sequential test, so interleaving
        // them with the server cases would serialize the inspect
        // processes.
        for (const fixture of corpus.fixtures) {
            test.skipIf(!fixture.active || fixture.meta.verify === "server").concurrent(
                `${corpus.feature}/${fixture.rel}`,
                async () => {
                    // checkInspectFixture throws on any failure, including
                    // a snapshot mismatch.
                    await expect(
                        checkInspectFixture(cliceExecutable(), corpus, fixture),
                    ).resolves.toBeUndefined();
                },
            );
        }

        for (const fixture of corpus.fixtures) {
            test.skipIf(!fixture.active || fixture.meta.verify === "inspect")(
                `${corpus.feature}/${fixture.rel} (server)`,
                async ({ session }) => {
                    await checkServerSnapFixture(session, corpus, fixture);
                },
            );
        }

        test("no orphan snapshots", () => {
            expect(orphanSnapshots(corpus)).toEqual([]);
        });
    });
}
