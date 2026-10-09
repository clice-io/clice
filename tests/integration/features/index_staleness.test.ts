/// Touching a header (mtime bump, identical content) must not reindex its
/// closed dependents — the content-hash staleness check is the storm filter.

import { MTIME_GRANULARITY, runProcess, sleep } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

const HEADER = "#pragma once\ninline int alpha() { return 1; }\n";
const CLOSED_TU = '#include "header.h"\nint use() { return alpha(); }\n';

/// Run a batch `clice index` over the workspace and return how many
/// translation units its summary reports indexing.
async function batchIndex(workspace: Workspace): Promise<number> {
    const exe = process.env["CLICE_EXECUTABLE"];
    if (!exe) {
        throw new Error("CLICE_EXECUTABLE is not set; point it at build/<type>/bin/bin/clice");
    }
    const run = await runProcess(exe, ["index", "--workspace", workspace.root], {
        timeout: 120_000,
    });
    expect(run.status, run.stderr).toBe(0);
    const match = /Indexed (\d+) translation unit/.exec(run.stdout);
    expect(match, `clice index reported no summary:\n${run.stdout}`).not.toBeNull();
    return Number(match![1]);
}

test("touch header no reindex", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.pinCacheDir();
    workspace.write("header.h", HEADER);
    workspace.write("closed.cpp", CLOSED_TU);
    workspace.writeCDB(["closed.cpp"]);

    // Run 1: index the closed TU into the database.
    expect(await batchIndex(workspace), "the first run indexes the closed TU").toBe(1);

    // Touch the header: bump mtime, keep the bytes identical.
    await sleep(MTIME_GRANULARITY);
    workspace.write("header.h", HEADER);

    // Run 2: the load re-enqueues every TU and runs the staleness check.
    // The touch makes the header's stat mismatch its FileVersion stamp;
    // the check re-hashes, proves a mere touch, and the storm filter
    // leaves the closed TU alone.
    expect(await batchIndex(workspace), "a same-content touch must not reindex dependents").toBe(0);
});

/// An importer's index reads the module's interface without entering its
/// file; a change to the module reindexes it all the same.
test("module change reindexes importer", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("m.cppm", "export module m;\nexport int alpha() { return 1; }\n");
    ws.write("closed.cpp", "import m;\nint use() { return alpha(); }\n");
    ws.writeCDB(["m.cppm", "closed.cpp"], { std: "c++20" });
    expect(await batchIndex(ws), "the first run indexes both units").toBe(2);

    await sleep(MTIME_GRANULARITY);
    ws.write("m.cppm", "export module m;\nexport int alpha() { return 1; }\n");
    expect(await batchIndex(ws), "a same-content touch reindexes nothing").toBe(0);

    await sleep(MTIME_GRANULARITY);
    ws.write("m.cppm", "export module m;\nexport int alpha() { return 2; }\n");
    expect(await batchIndex(ws), "the module and its importer reindex").toBe(2);
});
