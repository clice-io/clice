/// An index too large for an IPC message reaches the master through a
/// transfer file, and is dropped with a warning when there is no cache store
/// to hold one. CLICE_TEST_MAX_INDEX_BYTES lowers the inline limit so every
/// index here takes that route.

import type * as proto from "vscode-languageserver-protocol";
import { asLocations, runProcess, waitUntil, type CliceClient } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const HOOK = { CLICE_TEST_MAX_INDEX_BYTES: "1024" };
const FUNCTIONS = Array.from({ length: 200 }, (_, i) => `int function_${i}() { return ${i}; }`);
const BIG = `${FUNCTIONS.join("\n")}\nint use() { return function_0(); }\n`;

function files(locations: proto.Location[]): string[] {
    return locations.map((loc) => `${loc.uri.split("/").pop()}:${loc.range.start.line}`).sort();
}

function ownIndexWarnings(client: CliceClient, uri: string): string[] {
    return (client.diagnostics.get(uri) ?? [])
        .map((d) => (typeof d.message === "string" ? d.message : d.message.value))
        .filter((m) => m.includes("the file's own index"));
}

async function transfersRemoved(client: CliceClient): Promise<void> {
    await waitUntil(async () => (await client.stats()).pendingTmpFiles === 0, {
        timeout: 30_000,
        interval: 200,
        description: "transfer files left behind",
    });
}

test("open file index via file", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("big.cpp", BIG);
    ws.writeCDB(["big.cpp"]);
    const client = session.spawn(ws, { env: HOOK });
    await client.initialize(ws, {
        initializationOptions: { project: { enable_indexing: false } },
    });

    const [uri] = await client.openAndWait("big.cpp");
    expect(ownIndexWarnings(client, uri)).toEqual([]);
    expect(files((await client.referencesAt(uri, 0, 4)) ?? [])).toEqual([
        "big.cpp:0",
        "big.cpp:200",
    ]);
    await transfersRemoved(client);
});

test("background index via file", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("h.h", "#pragma once\nint foo(int a);\n");
    ws.write("main.cpp", '#include "h.h"\nint main() { return foo(1); }\n');
    ws.write("b.cpp", `#include "h.h"\nint foo(int a) { return a; }\n${BIG}`);
    ws.writeCDB(["main.cpp", "b.cpp"]);
    const client = session.spawn(ws, { env: HOOK });
    await client.initialize(ws);

    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "function_199"), "b.cpp not indexed").toBe(true);
    const column = "int main() { return ".length;
    expect(files(asLocations(await client.definitionAt(uri, 1, column)))).toEqual(["b.cpp:1"]);
    await transfersRemoved(client);
});

test("batch index via file", async ({ session }) => {
    const exe = cliceExecutable();
    const ws = session.tmpdir();
    ws.write("a.cpp", `int alpha() { return 1; }\n${BIG}`);
    ws.write("b.cpp", "int alpha();\nint beta() { return alpha(); }\n");
    ws.writeCDB(["a.cpp", "b.cpp"]);

    const env = { ...process.env, ...HOOK };
    const run = await runProcess(exe, ["index", "--workspace", ws.root], {
        env,
        timeout: 120_000,
    });
    expect(run.status, run.stderr).toBe(0);
    expect(run.stdout).toContain("Indexed 2 translation units");

    const shown = await runProcess(
        exe,
        ["index", "--workspace", ws.root, "--show-symbol", "alpha"],
        {
            env,
            timeout: 60_000,
        },
    );
    expect(shown.status, shown.stderr).toBe(0);
    expect(shown.stdout).toContain("reference files=2");
});

test("oversized index without store", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(".clice", "not a directory\n");
    ws.write("big.cpp", BIG);
    ws.writeCDB(["big.cpp"]);
    const client = session.spawn(ws, { env: HOOK });
    await client.initialize(ws);

    const [uri] = await client.openAndWait("big.cpp");
    const warnings = ownIndexWarnings(client, uri);
    expect(warnings.length).toBe(1);
    expect(warnings[0]).toContain("too large to send between clice processes");
    expect(await client.hoverAt(uri, 0, 5)).not.toBeNull();
});
