/// Provisional members: a source the user saves that no database lists
/// joins the build under a nearby unit's command — it provides its module,
/// hosts the headers it includes and is indexed — until a database lists it.

import * as fs from "node:fs";
import { runProcess, SETTLE_TIME, sleep, waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const A = "export module a;\nexport int fa() { return 1; }\n";
const B = "export module b;\nexport int fb() { return 2; }\n";
const IMPORTS_A = "import a;\nint main() { return fa(); }\n";
const IMPORTS_AB = "import a;\nimport b;\nint main() { return fa() + fb(); }\n";

/// Write a file the way an editor creates one: on disk, opened, saved.
async function saveNew(client: CliceClient, ws: Workspace, path: string, text: string) {
    ws.write(path, text);
    const [uri] = await client.openAndWait(path);
    client.save(uri);
    return uri;
}

/// Replace a document's buffer and wait for its recompile.
async function edit(client: CliceClient, uri: string, version: number, text: string) {
    client.change(uri, version, text);
    await client.waitForRecompile(uri);
}

function modules(session: { tmpdir(): Workspace }): Workspace {
    const ws = session.tmpdir();
    ws.write("a.cppm", A);
    ws.write("main.cpp", IMPORTS_A);
    ws.writeCDB(["a.cppm", "main.cpp"], { std: "c++20" });
    return ws;
}

async function symbolCount(client: CliceClient, name: string): Promise<number> {
    return ((await client.workspaceSymbols(name)) ?? []).filter((s) => s.name === name).length;
}

test("saved module is imported", async ({ session }) => {
    const ws = modules(session);
    const client = await session.spawn(ws).initialize(ws);
    const [main] = await client.openAndWait("main.cpp");
    await saveNew(client, ws, "b.cppm", B);
    await edit(client, main, 1, IMPORTS_AB);
    client.assertNoErrors(main, "the saved module provides b");
});

test("saved module survives restart", async ({ session }) => {
    const ws = modules(session);
    const first = await session.spawn(ws).initialize(ws);
    await saveNew(first, ws, "b.cppm", B);
    await first.shutdown();

    ws.write("main.cpp", IMPORTS_AB);
    const second = await session.spawn(ws).initialize(ws);
    const [main] = await second.openAndWait("main.cpp");
    second.assertNoErrors(main, "the record outlives the session");
});

test("saved partition joins module", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("a.cppm", "export module a;\nexport import :part;\nexport int fa() { return 1; }\n");
    ws.write("main.cpp", "import a;\nint main() { return fa() + fpart(); }\n");
    ws.writeCDB(["a.cppm", "main.cpp"], { std: "c++20" });
    const client = await session.spawn(ws).initialize(ws);
    const [main] = await client.openAndWait("main.cpp");
    client.assertHasErrors(main, "no file provides a:part yet");
    await saveNew(
        client,
        ws,
        "a-part.cppm",
        "export module a:part;\nexport int fpart() { return 3; }\n",
    );
    await client.waitForRecompile(main);
    client.assertNoErrors(main, "the partition completes the module");
});

test("deleted member comes back", async ({ session }) => {
    const ws = modules(session);
    ws.write("main.cpp", IMPORTS_AB);
    const client = await session.spawn(ws).initialize(ws);
    await saveNew(client, ws, "b.cppm", B);
    const [main] = await client.openAndWait("main.cpp");
    client.assertNoErrors(main);

    fs.rmSync(ws.path("b.cppm"));
    await client.poll("workspace");
    await client.waitForRecompile(main);
    client.assertHasErrors(main, "a deleted module provides nothing");

    ws.write("b.cppm", B);
    await client.poll("workspace");
    await client.waitForRecompile(main);
    client.assertNoErrors(main, "the record brings it back with the file");
});

test("closed member stays indexed", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("src/shared.h", "#pragma once\nint shared_fn();\n");
    ws.write("src/a.cpp", '#include "shared.h"\nint shared_fn() { return 0; }\n');
    ws.writeCDB(["src/a.cpp"]);
    const client = await session.spawn(ws).initialize(ws);
    const [a] = await client.openAndWait("src/a.cpp");
    const fresh = await saveNew(
        client,
        ws,
        "src/new.cpp",
        '#include "shared.h"\nint fresh_fn() { return shared_fn(); }\n',
    );
    client.close(fresh);
    expect(await client.waitForIndex(a, "fresh_fn"), "the member is indexed").toBe(true);
    expect(await client.waitForReference(a, 1, 5, ws.uri("src/new.cpp"))).toBe(true);
});

test("database takes over, then drops", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("src/a.cpp", "int a() { return 0; }\n");
    ws.writeCDB(["src/a.cpp"]);
    const client = await session.spawn(ws).initialize(ws);
    const [a] = await client.openAndWait("src/a.cpp");
    const text =
        "#ifndef LISTED\nint regen_fn() { return 1; }\n#else\nint listed_fn() { return 2; }\n#endif\n";
    client.close(await saveNew(client, ws, "src/new.cpp", text));
    expect(await client.waitForIndex(a, "regen_fn")).toBe(true);

    ws.writeEntries([
        ["src/a.cpp", []],
        ["src/new.cpp", ["-DLISTED"]],
    ]);
    await client.poll("cdb");
    expect(await client.waitForIndex(a, "listed_fn"), "the entry's command takes over").toBe(true);
    expect(await symbolCount(client, "regen_fn")).toBe(0);

    ws.writeCDB(["src/a.cpp"]);
    await client.poll("cdb");
    await waitUntil(async () => (await symbolCount(client, "listed_fn")) === 0, {
        timeout: 30_000,
        interval: 500,
        description: "the file leaving the index with its entry",
    });
    await sleep(SETTLE_TIME);
    expect(await symbolCount(client, "regen_fn"), "no record brings it back").toBe(0);
});

test("lost lender drops member", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("src/a.cpp", "int a() { return 0; }\n");
    ws.write("tools/legacy.c", "int legacy(void) { return 0; }\n");
    const entries = (withC: boolean) => [
        {
            directory: ws.root,
            file: ws.path("src/a.cpp"),
            arguments: ["clang++", ws.path("src/a.cpp")],
        },
        ...(withC
            ? [
                  {
                      directory: ws.root,
                      file: ws.path("tools/legacy.c"),
                      arguments: ["clang", ws.path("tools/legacy.c")],
                  },
              ]
            : []),
    ];
    ws.write("compile_commands.json", JSON.stringify(entries(true)));
    const client = await session.spawn(ws).initialize(ws);
    const [a] = await client.openAndWait("src/a.cpp");
    client.close(await saveNew(client, ws, "tools/tool.c", "int tool_fn(void) { return 1; }\n"));
    expect(await client.waitForIndex(a, "tool_fn")).toBe(true);

    ws.write("compile_commands.json", JSON.stringify(entries(false)));
    await client.poll("cdb");
    await waitUntil(async () => (await symbolCount(client, "tool_fn")) === 0, {
        timeout: 30_000,
        interval: 500,
        description: "a .c with no C unit left to borrow from leaving the index",
    });
});

test("member hosts its headers", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("lib/x.cpp", "int x() { return 0; }\n");
    ws.write("lib/frag.h", "Ctx use();\n");
    ws.writeCDB(["lib/x.cpp"]);
    const client = await session.spawn(ws).initialize(ws);
    await saveNew(client, ws, "lib/new.cpp", 'struct Ctx {};\n#include "frag.h"\n');
    const [frag] = await client.openAndWait("lib/frag.h");
    client.assertNoErrors(frag, "the member lends its context to the header");
});

test("included source keeps host", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("unity.cpp", '#define UNITY 1\n#include "part.cpp"\n');
    const part = "#ifndef UNITY\n#error needs the unity file\n#endif\nint part() { return 0; }\n";
    ws.write("part.cpp", part);
    ws.writeCDB(["unity.cpp"]);
    const client = await session.spawn(ws).initialize(ws);
    const [uri] = await client.openAndWait("part.cpp");
    client.assertNoErrors(uri);
    client.save(uri);
    await edit(client, uri, 1, part + "int more() { return 1; }\n");
    client.assertNoErrors(uri, "a source another unit includes is no member of its own");
});

test("query sees the member", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("src/a.cpp", "int a() { return 0; }\n");
    ws.writeCDB(["src/a.cpp"]);
    ws.pinCacheDir();
    const client = await session.spawn(ws).initialize(ws);
    await saveNew(client, ws, "src/new.cpp", "int fresh_fn() { return 1; }\n");
    await client.shutdown();

    const run = await runProcess(
        cliceExecutable(),
        ["query", "--workspace", ws.root, "--method", "compileCommand", "--path", "src/new.cpp"],
        { timeout: 120_000 },
    );
    const answer = JSON.parse(run.stdout) as { result?: { source: string } };
    expect(answer.result?.source, run.stderr).toBe("provisional");
});

test("first module borrows project flags", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write(
        "include/util.h",
        "#pragma once\n#ifndef FROM_A\n#error needs FROM_A\n#endif\ninline int util() { return 1; }\n",
    );
    ws.write("src/a.cpp", "int a() { return 0; }\n");
    ws.writeCDB(["src/a.cpp"], {
        std: "c++20",
        extraArgs: ["-DFROM_A", `-I${ws.path("include")}`],
    });
    const client = await session.spawn(ws).initialize(ws);
    const first = await saveNew(
        client,
        ws,
        "src/first.cppm",
        'module;\n#include "util.h"\nexport module first;\nexport int f() { return util(); }\n',
    );
    client.assertNoErrors(first, "the module compiles under the .cpp unit's flags");
    const [a] = await client.openAndWait("src/a.cpp");
    await edit(client, a, 1, "import first;\nint a() { return f(); }\n");
    client.assertNoErrors(a, "the database unit imports it");
});
