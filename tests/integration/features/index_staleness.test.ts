/// Touching a header (mtime bump, identical content) must not reindex its
/// closed dependents — the content-hash staleness check is the storm filter.

import * as fs from "node:fs";
import * as path from "node:path";
import { MTIME_GRANULARITY, sleep, waitUntil } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

/// The probes below watch per-file blob mtimes, so the sessions pin the
/// files backend (the default LMDB backend has no per-blob files).
const NO_LMDB = { project: { index_db: "files" } };

const HEADER = "#pragma once\ninline int alpha() { return 1; }\n";
const CLOSED_TU = '#include "header.h"\nint use() { return alpha(); }\n';

/// mtimes of the per-file shard blobs (the "index" namespace holds nothing
/// else).
function shardMtimes(workspace: Workspace): Map<string, bigint> {
    const dir = path.join(workspace.cacheRoot(), "index");
    const shards = new Map<string, bigint>();
    if (!fs.existsSync(dir)) {
        return shards;
    }
    for (const name of fs.readdirSync(dir)) {
        if (name.endsWith(".idx")) {
            shards.set(name, fs.statSync(path.join(dir, name), { bigint: true }).mtimeNs);
        }
    }
    return shards;
}

function globalMtime(workspace: Workspace): bigint {
    const p = path.join(workspace.cacheRoot(), "index-global", "global.idx");
    return fs.existsSync(p) ? fs.statSync(p, { bigint: true }).mtimeNs : 0n;
}

test("touch header no reindex", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("header.h", HEADER);
    workspace.write("closed.cpp", CLOSED_TU);
    workspace.writeCDB(["closed.cpp"]);

    // Session 1: background-index the closed TU into a shard.
    const c1 = session.spawn(workspace);
    await c1.initialize(workspace, { initializationOptions: NO_LMDB });
    await waitUntil(() => shardMtimes(workspace).size > 0, {
        timeout: 30_000,
        interval: 1_000,
        description: "the closed translation unit to be indexed",
    });
    await c1.shutdown();

    // Touch the header: bump mtime, keep the bytes identical.
    await sleep(MTIME_GRANULARITY);
    workspace.write("header.h", HEADER);

    // Session 2: restart re-enqueues every TU and runs the staleness check.
    // Snapshot the shards BEFORE starting the session — startup indexing is
    // already running when initialize returns, so a later snapshot could
    // race a (buggy) reindex and pass vacuously. Wait until the round
    // completes (save() rewrites the project blob), then re-snapshot: the
    // storm filter must skip the closed TU, leaving its shard untouched.
    const before = shardMtimes(workspace);
    const globalBefore = globalMtime(workspace);
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace, { initializationOptions: NO_LMDB });
    // The touch makes the header's stat mismatch its FileVersion stamp; the
    // staleness check re-hashes, proves a mere touch, and repairs the stamp
    // — which dirties the global blob, so its mtime moving proves both that
    // the round ran and that the repair persisted.
    await waitUntil(() => globalMtime(workspace) !== globalBefore, {
        timeout: 30_000,
        interval: 1_000,
        description: "the second session's indexing round to persist",
    });
    const after = shardMtimes(workspace);
    await c2.shutdown();

    expect(after, "a same-content touch must not reindex dependents").toEqual(before);
});
