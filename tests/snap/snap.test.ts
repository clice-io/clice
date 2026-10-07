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

// CI splits the suite over runners: CLICE_SNAP_SHARD=<index>/<count> keeps
// every count-th fixture from the index-th. vitest's own --shard splits by
// file, and this suite is one file.
const shardSpec = process.env["CLICE_SNAP_SHARD"] ?? "1/1";
const shard = /^(\d+)\/(\d+)$/.exec(shardSpec);
const shardIndex = Number(shard?.[1]);
const shardCount = Number(shard?.[2]);
if (shard === null || shardIndex < 1 || shardIndex > shardCount) {
    throw new Error(`CLICE_SNAP_SHARD must be <index>/<count>, not '${shardSpec}'`);
}
let ordinal = 0;

for (const corpus of snapCorpora()) {
    feature(corpus.feature); // every corpus must be registered
    const fixtures = corpus.fixtures.filter(
        (_, i) => (ordinal + i) % shardCount === shardIndex - 1,
    );
    ordinal += corpus.fixtures.length;
    describe(`snap/${corpus.feature}`, () => {
        // All inspect cases register contiguously: vitest closes a
        // concurrent batch at the first sequential test, so interleaving
        // them with the server cases would serialize the inspect
        // processes.
        for (const fixture of fixtures) {
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

        for (const fixture of fixtures) {
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
