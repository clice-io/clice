/// The pch_build policy: reads are answered from the index, PCH/AST
/// investment follows the configured trigger. Each degraded surface the
/// design pins (empty inlay hints, no diagnostics push before a trigger)
/// is asserted explicitly.

import * as proto from "vscode-languageserver-protocol";
import type { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

const HEADER = "#pragma once\nint add(int a, int b);\n";
const MAIN = [
    '#include "header.h"',
    "",
    "/// Doubles a value.",
    "int twice(int x) {",
    "    return add(x, x);",
    "}",
    "",
    "int main() { return twice(2); }",
    "",
].join("\n");

function writeProject(session: { tmpdir(): Workspace }): Workspace {
    const ws = session.tmpdir();
    ws.write("header.h", HEADER);
    ws.write("main.cpp", MAIN);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    return ws;
}

const ON_EDIT = { project: { pch_build: "on_edit" } };

function labelsOf(result: proto.CompletionItem[] | proto.CompletionList | null): string[] {
    if (result === null) {
        return [];
    }
    const items = Array.isArray(result) ? result : result.items;
    return items.map((item) => item.label);
}

test("index serves unedited reads", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: ON_EDIT });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);

    const tokens = await client.semanticTokensFull(uri);
    expect(tokens?.data.length ?? 0).toBeGreaterThan(0);

    const symbols = (await client.documentSymbols(uri)) as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("twice");

    const folds = await client.foldingRanges(uri);
    expect(folds?.length ?? 0).toBeGreaterThan(0);

    const links = await client.documentLinks(uri);
    expect(links?.some((l) => l.target?.endsWith("header.h"))).toBe(true);

    // `add` in `return add(x, x)` on line 4.
    const hover = await client.hoverAt(uri, 4, 11);
    expect(JSON.stringify(hover?.contents ?? "")).toContain("add");

    const defs = (await client.definitionAt(uri, 4, 11)) as proto.Location[] | null;
    expect(defs?.length ?? 0).toBeGreaterThan(0);
    expect(defs![0]!.uri.endsWith("header.h")).toBe(true);

    // Pinned degradations of the read-only surface.
    const hints = await client.inlayHints(uri, {
        start: { line: 0, character: 0 },
        end: { line: 8, character: 0 },
    });
    expect(hints ?? []).toEqual([]);
    expect(client.diagnostics.has(uri)).toBe(false);

    // Reading never builds a PCH.
    expect(ws.pchFiles()).toEqual([]);
});

test("edit escalates to compile", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: ON_EDIT });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);
    expect(client.diagnostics.has(uri)).toBe(false);

    // The edit is the trigger: diagnostics arrive without any feature
    // request pulling the compile.
    const arrived = client.armDiagnostics(uri);
    client.change(uri, 2, MAIN + "// edited\n");
    await arrived;
    client.assertNoErrors(uri);
});

test("diverged open buffer escalates", async ({ session }) => {
    const ws = writeProject(session);

    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    const first = session.spawn(ws);
    await first.initialize(ws, { initializationOptions: ON_EDIT });
    const [warm] = first.open("main.cpp");
    expect(await first.waitForIndex(warm, "twice")).toBe(true);
    await first.shutdown();

    const second = session.spawn(ws);
    await second.initialize(ws, { initializationOptions: ON_EDIT });

    // A restored unsaved buffer diverges from the indexed content: the
    // open itself escalates, diagnostics arrive with no edit and no
    // feature request.
    const uri = ws.uri("main.cpp");
    const arrived = second.armDiagnostics(uri);
    second.open("main.cpp", 0, { text: MAIN + "// restored, unsaved\n" });
    await arrived;
    second.assertNoErrors(uri);
});

test("never builds no pch", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, {
        initializationOptions: { project: { pch_build: "never" } },
    });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);

    // Completion still answers — a full parse without a preamble.
    const completion = await client.completionAt(uri, 7, 22);
    expect(labelsOf(completion)).toContain("twice");

    // The whole point of the profile.
    expect(ws.pchFiles()).toEqual([]);
    expect(client.diagnostics.has(uri)).toBe(false);
});

test("escalation upgrades inlay hints", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: ON_EDIT });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);

    const range = {
        start: { line: 0, character: 0 },
        end: { line: 8, character: 0 },
    };
    expect((await client.inlayHints(uri, range)) ?? []).toEqual([]);

    // After the edit-triggered compile lands, a re-pull gets real hints
    // (parameter names at the call sites).
    const arrived = client.armDiagnostics(uri);
    client.change(uri, 2, MAIN + "// edited\n");
    await arrived;
    const upgraded = await client.inlayHints(uri, range);
    expect(upgraded?.length ?? 0).toBeGreaterThan(0);
});

test("diverged buffer serves no links", async ({ session }) => {
    const ws = writeProject(session);

    const first = session.spawn(ws);
    await first.initialize(ws, { initializationOptions: ON_EDIT });
    const [warm] = first.open("main.cpp");
    expect(await first.waitForIndex(warm, "twice")).toBe(true);
    await first.shutdown();

    // Under `never` a diverged buffer cannot escalate: every index answer
    // must withdraw rather than map stale manifest lines onto new text.
    const second = session.spawn(ws);
    await second.initialize(ws, {
        initializationOptions: { project: { pch_build: "never" } },
    });
    const [uri] = second.open("main.cpp", 0, {
        text: '#include "renamed.h"\n' + MAIN.split("\n").slice(1).join("\n"),
    });
    expect((await second.documentLinks(uri)) ?? []).toEqual([]);
    const defs = await second.definitionAt(uri, 0, 12);
    expect(defs === null || (Array.isArray(defs) && defs.length === 0)).toBe(true);
});

const ON_OPEN = { project: { pch_build: "on_open" } };

test("preamble define hovers under pch", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("header.h", HEADER);
    ws.write("main.cpp", "#define LIMIT 10\n" + MAIN);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: ON_OPEN });

    // The define is compiled into the PCH and has no AST node; the null
    // from the worker falls back to the index card for the preamble
    // region (and only there).
    await client.openAndWait("main.cpp");
    const uri = ws.uri("main.cpp");
    const hover = await client.hoverAt(uri, 0, 9);
    expect(JSON.stringify(hover?.contents ?? "")).toContain("LIMIT");
});

test("on_open compiles eagerly", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: ON_OPEN });

    // No feature request at all: didOpen itself starts the compile.
    const uri = ws.uri("main.cpp");
    const arrived = client.armDiagnostics(uri);
    client.open("main.cpp");
    await arrived;
    client.assertNoErrors(uri);
});

test("index answers while eager compile runs", async ({ session }) => {
    const ws = writeProject(session);

    const first = session.spawn(ws);
    await first.initialize(ws, { initializationOptions: ON_EDIT });
    const [warm] = first.open("main.cpp");
    expect(await first.waitForIndex(warm, "twice")).toBe(true);
    await first.shutdown();

    // on_open: the compile is kicked at didOpen, but the very first
    // request must not wait for it — the warm shard answers.
    const second = session.spawn(ws);
    await second.initialize(ws, { initializationOptions: ON_OPEN });
    const [uri] = second.open("main.cpp");
    const symbols = (await second.documentSymbols(uri)) as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("twice");
});
