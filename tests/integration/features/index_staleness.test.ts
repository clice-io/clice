/// Touching a header (mtime bump, identical content) must not reindex its
/// closed dependents — the content-hash staleness check is the storm filter.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import type * as proto from "vscode-languageserver-protocol";
import { asLocations, MTIME_GRANULARITY, sleep } from "@clice/tools/client";
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

test("deleted source withdraws its rows", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("h.h", "#pragma once\nint foo(int a);\n");
    ws.write("main.cpp", '#include "h.h"\nint main() { return foo(1); }\n');
    ws.write(
        "b.cpp",
        '#include "h.h"\nint foo(int a) { return a; }\nint only_in_b() { return foo(2); }\n',
    );
    ws.writeCDB(["main.cpp", "b.cpp"]);
    const client = session.spawn(ws);
    await client.initialize(ws);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "only_in_b"), "b.cpp not indexed").toBe(true);

    const files = (locations: proto.Location[]) =>
        locations.map((loc) => `${loc.uri.split("/").pop()}:${loc.range.start.line}`).sort();
    const column = "int main() { return ".length;
    expect(files(asLocations(await client.definitionAt(uri, 1, column)))).toEqual(["b.cpp:1"]);

    // The CDB still lists b.cpp; its rows describe text that is gone.
    fs.rmSync(ws.path("b.cpp"));
    await client.poll("workspace");
    expect(files(asLocations(await client.definitionAt(uri, 1, column)))).toEqual(["h.h:1"]);
    expect(files((await client.referencesAt(uri, 1, column)) ?? [])).toEqual([
        "h.h:1",
        "main.cpp:1",
    ]);
    expect(await client.workspaceSymbols("only_in_b")).toEqual([]);
    await client.shutdown();

    const exe = process.env["CLICE_EXECUTABLE"]!;
    const search = JSON.parse(
        execFileSync(
            exe,
            ["query", "--workspace", ws.root, "--method", "symbolSearch", "--query", "only_in_b"],
            { encoding: "utf8", timeout: 120_000 },
        ),
    ) as { result: { symbols: unknown[] }; stale: string[] };
    expect(search.result.symbols).toEqual([]);
});
