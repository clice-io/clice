/// Provisional members: a source the user saves that no database lists
/// joins the build under a nearby unit's command — it provides its module,
/// hosts the headers it includes and is indexed — until a database lists it.

import { runProcess, waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const A = "export module a;\nexport int fa() { return 1; }\n";
const B = "export module b;\nexport int fb() { return 2; }\nint b_local() { return 3; }\n";
const IMPORTS_A = "import a;\nint main() { return fa(); }\n";
const IMPORTS_AB = "import a;\nimport b;\nint main() { return fa() + fb(); }\n";
const PART = "#ifndef UNITY\n#error needs the unity file\n#endif\nint part() { return 0; }\n";
const UNITY = '#define UNITY 1\n#include "part.cpp"\n';

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

async function reload(client: CliceClient): Promise<void> {
    expect((await client.poll("cdb", { force: true })).events, "the reload changes the build").toBe(
        1,
    );
}

/// What `clice query compileCommand` says a file's command came from.
async function commandSource(ws: Workspace, path: string): Promise<string | undefined> {
    const run = await runProcess(
        cliceExecutable(),
        ["query", "--workspace", ws.root, "--method", "compileCommand", "--path", path],
        { timeout: 120_000 },
    );
    return (JSON.parse(run.stdout) as { result?: { source: string } }).result?.source;
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
    expect(await second.waitForIndex(main, "b_local"), "the restored member is indexed").toBe(true);
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

    ws.rm("b.cppm");
    await client.poll("workspace");
    await client.waitForRecompile(main);
    client.assertHasErrors(main, "a deleted module provides nothing");

    // The reload rebuilds the graph from the build's units: only the kept
    // record still counts the missing file among them.
    ws.write("c.cpp", "int c() { return 0; }\n");
    ws.writeCDB(["a.cppm", "main.cpp", "c.cpp"], { std: "c++20" });
    await reload(client);

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
    ws.write("src/s.cpp", "int s() { return 0; }\n");
    ws.writeCDB(["src/a.cpp", "src/s.cpp"]);
    const client = await session.spawn(ws).initialize(ws, {
        initializationOptions: { project: { idle_timeout_ms: 10 } },
    });
    const [a] = await client.openAndWait("src/a.cpp");
    const text =
        "#ifndef LISTED\nint regen_fn() { return 1; }\n#else\nint listed_fn() { return 2; }\n#endif\n";
    client.close(await saveNew(client, ws, "src/new.cpp", text));
    expect(await client.waitForIndex(a, "regen_fn")).toBe(true);

    ws.writeEntries([
        ["src/a.cpp", []],
        ["src/s.cpp", []],
        ["src/new.cpp", ["-DLISTED"]],
    ]);
    await reload(client);
    expect(await client.waitForIndex(a, "listed_fn"), "the entry's command takes over").toBe(true);
    expect(await symbolCount(client, "regen_fn")).toBe(0);

    ws.writeCDB(["src/a.cpp", "src/s.cpp"]);
    await reload(client);
    await waitUntil(async () => (await symbolCount(client, "listed_fn")) === 0, {
        timeout: 30_000,
        interval: 500,
        description: "the file leaving the index with its entry",
    });
    // Queued after the reload: a record outliving the takeover would have
    // the file indexed again by the time this lands.
    ws.write("src/s.cpp", "int s() { return 0; }\nint sentinel_fn() { return 3; }\n");
    await client.poll("workspace");
    expect(await client.waitForIndex(a, "sentinel_fn")).toBe(true);
    expect(await symbolCount(client, "regen_fn"), "no record brings it back").toBe(0);

    const [again] = await client.openAndWait("src/new.cpp");
    client.save(again);
    client.close(again);
    expect(await client.waitForIndex(a, "regen_fn"), "a save records it again").toBe(true);
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
    await reload(client);
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
    const [frag] = await client.openAndWait("lib/frag.h");
    client.assertHasErrors(frag, "nothing provides Ctx yet");
    await saveNew(client, ws, "lib/new.cpp", 'struct Ctx {};\n#include "frag.h"\n');
    await client.waitForRecompile(frag);
    client.assertNoErrors(frag, "the member lends its context to the header");
});

test("included source keeps host", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("unity.cpp", UNITY);
    ws.writeCDB(["unity.cpp"]);
    const first = await session.spawn(ws).initialize(ws);
    const [uri] = await first.openAndWait("unity.cpp");
    first.assertHasErrors(uri, "part.cpp does not exist yet");
    const part = await saveNew(first, ws, "part.cpp", PART);
    await edit(first, part, 1, PART + "int more() { return 1; }\n");
    first.assertNoErrors(part, "a source a unit includes is no member of its own");
    await first.shutdown();

    const second = await session.spawn(ws).initialize(ws);
    const [restored] = await second.openAndWait("part.cpp");
    second.assertNoErrors(restored, "nor after a restart");
});

test("regenerated unity takes member", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("other.cpp", "int other() { return 0; }\n");
    ws.writeCDB(["other.cpp"]);
    const client = await session.spawn(ws).initialize(ws);
    const part = await saveNew(client, ws, "part.cpp", PART);
    client.assertHasErrors(part, "a member of its own, without the unity file");
    client.close(part);

    ws.write("unity.cpp", UNITY);
    ws.writeCDB(["other.cpp", "unity.cpp"]);
    await reload(client);
    const [reopened] = await client.openAndWait("part.cpp");
    client.assertNoErrors(reopened, "the regenerated unity file hosts it");
});

test("query labels the members", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("src/a.cpp", "int a() { return 0; }\n");
    ws.write("out/CMakeCache.txt", "");
    ws.writeCDB(["src/a.cpp"]);
    ws.pinCacheDir();
    const client = await session.spawn(ws).initialize(ws);
    await saveNew(client, ws, "src/new.cpp", "int fresh_fn() { return 1; }\n");
    await saveNew(client, ws, "src/orphan.h", "#pragma once\n");
    await saveNew(client, ws, "out/gen.cpp", "int gen() { return 0; }\n");
    ws.write("src/opened.cpp", "int opened() { return 0; }\n");
    await client.openAndWait("src/opened.cpp");
    await client.shutdown();

    expect(await commandSource(ws, "src/new.cpp")).toBe("provisional");
    expect(await commandSource(ws, "src/orphan.h"), "a header is never one").toBe("inferred");
    expect(await commandSource(ws, "out/gen.cpp"), "a build tree holds none").toBe("inferred");
    expect(await commandSource(ws, "src/opened.cpp"), "opening records nothing").toBe("inferred");
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

test("implementation unit borrows interface", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("src/main.cpp", "int main() { return 0; }\n");
    ws.write("mods/m.cppm", "export module m;\nexport int mv();\n");
    ws.writeEntries(
        [
            ["src/main.cpp", []],
            ["mods/m.cppm", ["-DFROM_MODS"]],
        ],
        { std: "c++20" },
    );
    const impl =
        "module m;\n#ifndef FROM_MODS\n#error needs the interface's flags\n#endif\nint mv() { return 1; }\n";
    const client = await session.spawn(ws).initialize(ws);
    const uri = await saveNew(client, ws, "mods/m_impl.cpp", impl);
    client.assertNoErrors(uri, "the interface next to it lends");
    await edit(client, uri, 1, impl + "int twice() { return mv() + mv(); }\n");
    client.assertNoErrors(uri, "and keeps lending once it is a member");
});
