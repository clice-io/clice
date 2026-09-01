/// Touching a header (mtime bump, identical content) must not reindex its
/// closed dependents — the content-hash staleness check is the storm filter.

import { execFileSync } from "node:child_process";
import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

const HEADER = "#pragma once\ninline int alpha() { return 1; }\n";
const CLOSED_TU = '#include "header.h"\nint use() { return alpha(); }\n';

/// Run a batch `clice index` over the workspace and return how many
/// translation units its summary reports indexing.
function batchIndex(workspace: Workspace): number {
    const exe = process.env["CLICE_EXECUTABLE"];
    if (!exe) {
        throw new Error("CLICE_EXECUTABLE is not set; point it at build/<type>/bin/clice");
    }
    const out = execFileSync(exe, ["index", "--workspace", workspace.root], {
        encoding: "utf8",
        timeout: 120_000,
    });
    const match = /Indexed (\d+) translation unit/.exec(out);
    expect(match, `clice index reported no summary:\n${out}`).not.toBeNull();
    return Number(match![1]);
}

test("touch header no reindex", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.pinCacheDir();
    workspace.write("header.h", HEADER);
    workspace.write("closed.cpp", CLOSED_TU);
    workspace.writeCDB(["closed.cpp"]);

    // Run 1: index the closed TU into the database.
    expect(batchIndex(workspace), "the first run indexes the closed TU").toBe(1);

    // Touch the header: bump mtime, keep the bytes identical.
    await sleep(MTIME_GRANULARITY);
    workspace.write("header.h", HEADER);

    // Run 2: the load re-enqueues every TU and runs the staleness check.
    // The touch makes the header's stat mismatch its FileVersion stamp;
    // the check re-hashes, proves a mere touch, and the storm filter
    // leaves the closed TU alone.
    expect(batchIndex(workspace), "a same-content touch must not reindex dependents").toBe(0);
});
