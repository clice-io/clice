/// Touching a header (mtime bump, identical content) must not reindex its
/// closed dependents — the content-hash staleness check is the storm filter.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { MTIME_GRANULARITY, sleep } from "../../tools/checks.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";

const HEADER = "#pragma once\ninline int alpha() { return 1; }\n";
const CLOSED_TU = '#include "header.h"\nint use() { return alpha(); }\n';

/// Versioned root of the unified cache store; bump together with
/// cache_format_version in src/server/state/workspace.h.
function cacheRoot(workspace: string): string {
    return path.join(workspace, ".clice", "cache", "v4");
}

/// mtimes of the per-TU shards (numeric names; excludes the project blob).
function shardMtimes(workspace: string): Map<string, bigint> {
    const dir = path.join(cacheRoot(workspace), "index");
    const shards = new Map<string, bigint>();
    if (!fs.existsSync(dir)) {
        return shards;
    }
    for (const name of fs.readdirSync(dir)) {
        if (name.endsWith(".idx") && name.slice(0, -".idx".length) !== "project") {
            shards.set(name, fs.statSync(path.join(dir, name), { bigint: true }).mtimeNs);
        }
    }
    return shards;
}

function projectMtime(workspace: string): bigint {
    const p = path.join(cacheRoot(workspace), "index", "project.idx");
    return fs.existsSync(p) ? fs.statSync(p, { bigint: true }).mtimeNs : 0n;
}

async function poll(predicate: () => boolean, timeoutSeconds = 30): Promise<boolean> {
    for (let i = 0; i < timeoutSeconds; i++) {
        if (predicate()) {
            return true;
        }
        await sleep(1_000);
    }
    return false;
}

test("touch header no reindex", async () => {
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
    try {
        fs.writeFileSync(path.join(workspace, "header.h"), HEADER);
        fs.writeFileSync(path.join(workspace, "closed.cpp"), CLOSED_TU);
        writeCdb(workspace, ["closed.cpp"]);

        const executable = cliceExecutable();

        // Session 1: background-index the closed TU into a shard.
        const c1 = await makeClient(executable, workspace);
        expect(await poll(() => shardMtimes(workspace).size > 0), "closed TU never indexed").toBe(
            true,
        );
        await shutdownClient(c1);

        // Touch the header: bump mtime, keep the bytes identical.
        await sleep(MTIME_GRANULARITY);
        fs.writeFileSync(path.join(workspace, "header.h"), HEADER);

        // Session 2: restart re-enqueues every TU and runs the staleness check.
        // Snapshot the shards BEFORE starting the session — startup indexing is
        // already running when make_client returns, so a later snapshot could
        // race a (buggy) reindex and pass vacuously. Wait until the round
        // completes (save() rewrites the project blob), then re-snapshot: the
        // storm filter must skip the closed TU, leaving its shard untouched.
        const before = shardMtimes(workspace);
        const projectBefore = projectMtime(workspace);
        const c2 = await makeClient(executable, workspace);
        // save() rewrites the project blob unconditionally each round (see
        // Indexer::save), so its mtime moving proves the round ran.
        expect(
            await poll(() => projectMtime(workspace) !== projectBefore),
            "indexing round never ran in session 2",
        ).toBe(true);
        const after = shardMtimes(workspace);
        await shutdownClient(c2);

        expect(after, "a same-content touch must not reindex dependents").toEqual(before);
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});
