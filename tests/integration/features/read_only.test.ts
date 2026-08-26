/// The readonly serving modes: reads are answered from the index while
/// PCH/AST builds stay pull-driven — the mode only decides whether they
/// are a goal at all. Each degraded surface the design pins (empty inlay
/// hints, no diagnostics push before a pull) is asserted explicitly.

import * as proto from "vscode-languageserver-protocol";
import { SETTLE_TIME, sleep } from "@clice/tools/client";
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

const AUTO = { project: { readonly: "auto" } };

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
    await client.initialize(ws, { initializationOptions: AUTO });

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

test("oversized buffer keeps row answers", async ({ session }) => {
    const ws = session.tmpdir();
    // Past the 8 MiB full-lex cap only semantic tokens and folds follow
    // the investment policy; row- and cursor-backed projections still
    // serve from the shard.
    ws.write("header.h", HEADER);
    ws.write("main.cpp", MAIN + "// padding\n".repeat(800_000));
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: AUTO });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);

    const symbols = (await client.documentSymbols(uri)) as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("twice");

    const links = await client.documentLinks(uri);
    expect(links?.some((l) => l.target?.endsWith("header.h"))).toBe(true);

    // `add` in `return add(x, x)` on line 4.
    const hover = await client.hoverAt(uri, 4, 11);
    expect(JSON.stringify(hover?.contents ?? "")).toContain("add");

    expect(await client.semanticTokensFull(uri)).toBeNull();
    expect(await client.foldingRanges(uri)).toEqual([]);
    expect(ws.pchFiles()).toEqual([]);
});

test("cold outline awaits the boost", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: AUTO });

    // No waitForIndex: outline and links have no refresh request, so the
    // replies themselves await the didOpen boost instead of freezing an
    // empty result in the client's cache.
    const [uri] = client.open("main.cpp");
    const [symbolsRaw, links] = await Promise.all([
        client.documentSymbols(uri),
        client.documentLinks(uri),
    ]);
    const symbols = symbolsRaw as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("twice");
    expect(links?.some((l) => l.target?.endsWith("header.h"))).toBe(true);
});

test("edit escalates to compile", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: AUTO });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);
    expect(client.diagnostics.has(uri)).toBe(false);

    // The edit flips the mode; the build itself stays pull-driven, so
    // nothing lands until the next read pulls it.
    let published = false;
    const arrived = client.armDiagnostics(uri).then(() => {
        published = true;
    });
    client.change(uri, 2, MAIN + "// edited\n");
    await sleep(SETTLE_TIME);
    expect(published).toBe(false);

    const hover = await client.hoverAt(uri, 4, 11);
    expect(JSON.stringify(hover?.contents ?? "")).toContain("add");
    await arrived;
    client.assertNoErrors(uri);
});

test("diverged open buffer escalates", async ({ session }) => {
    const ws = writeProject(session);

    // Warm the index, then restart: the second server starts with the
    // shard on disk and nothing compiled.
    const first = session.spawn(ws);
    await first.initialize(ws, { initializationOptions: AUTO });
    const [warm] = first.open("main.cpp");
    expect(await first.waitForIndex(warm, "twice")).toBe(true);
    await first.shutdown();

    const second = session.spawn(ws);
    await second.initialize(ws, { initializationOptions: AUTO });

    // A restored unsaved buffer diverges from the indexed content: the
    // open itself escalates, so the first read pulls a compile instead
    // of answering empty from a withdrawn shard.
    const uri = ws.uri("main.cpp");
    const arrived = second.armDiagnostics(uri);
    second.open("main.cpp", 0, { text: MAIN + "// restored, unsaved\n" });
    const symbols = (await second.documentSymbols(uri)) as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("twice");
    await arrived;
    second.assertNoErrors(uri);
});

test("unservable boost escalates", async ({ session }) => {
    const ws = writeProject(session);
    // orphan.cpp is on disk but outside the CDB with no includer, so
    // indexing refuses the guessed command; scratch.cpp has a CDB entry
    // but exists only as the didOpen buffer, and indexing reads disk
    // truth. Neither boost can deliver a shard: the failed attempt
    // escalates, and the parked outline re-routes to a pulled compile.
    ws.write("orphan.cpp", "int orphan() { return 1; }\n");
    ws.writeCDB(["main.cpp", "scratch.cpp"]);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: AUTO });

    const orphanArrived = client.armDiagnostics(ws.uri("orphan.cpp"));
    const [orphanUri] = client.open("orphan.cpp");
    const orphan = (await client.documentSymbols(orphanUri)) as proto.DocumentSymbol[] | null;
    expect(orphan?.map((s) => s.name)).toContain("orphan");
    await orphanArrived;
    client.assertNoErrors(orphanUri);

    const scratchArrived = client.armDiagnostics(ws.uri("scratch.cpp"));
    const [scratchUri] = client.open("scratch.cpp", 0, { text: "int scratch() { return 2; }\n" });
    const scratch = (await client.documentSymbols(scratchUri)) as proto.DocumentSymbol[] | null;
    expect(scratch?.map((s) => s.name)).toContain("scratch");
    await scratchArrived;
    client.assertNoErrors(scratchUri);
});

test("explicit -x beats the suffix", async ({ session }) => {
    const ws = session.tmpdir();
    // C++ code behind a C suffix, forced by the CDB's -x: the index
    // projection must lex with the forced dialect — under the C keyword
    // table `class` would go unpainted and the first token would start at
    // `Widget` instead.
    ws.write("legacy.c", "class Widget { public: int value; };\n");
    ws.writeCDB(["legacy.c"], { extraArgs: ["-x", "c++"] });
    ws.pinCacheDir();
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: AUTO });

    const [uri] = client.open("legacy.c");
    expect(await client.waitForIndex(uri, "Widget")).toBe(true);
    const tokens = await client.semanticTokensFull(uri);
    expect(tokens?.data.slice(0, 2)).toEqual([0, 0]);
    expect(ws.pchFiles()).toEqual([]);
});

test("readonly on builds no pch", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, {
        initializationOptions: { project: { readonly: "on" } },
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
    await client.initialize(ws, { initializationOptions: AUTO });

    const [uri] = client.open("main.cpp");
    expect(await client.waitForIndex(uri, "twice")).toBe(true);

    const range = {
        start: { line: 0, character: 0 },
        end: { line: 8, character: 0 },
    };
    expect((await client.inlayHints(uri, range)) ?? []).toEqual([]);

    // The edit flips the mode: the inlay re-pull rides the pulled
    // compile and answers from the AST (parameter names at call sites).
    client.change(uri, 2, MAIN + "// edited\n");
    const upgraded = await client.inlayHints(uri, range);
    expect(upgraded?.length ?? 0).toBeGreaterThan(0);
});

test("diverged buffer serves no links", async ({ session }) => {
    const ws = writeProject(session);

    const first = session.spawn(ws);
    await first.initialize(ws, { initializationOptions: AUTO });
    const [warm] = first.open("main.cpp");
    expect(await first.waitForIndex(warm, "twice")).toBe(true);
    await first.shutdown();

    // Under readonly "on" a diverged buffer cannot escalate: every index
    // answer must withdraw rather than map stale manifest lines onto new
    // text.
    const second = session.spawn(ws);
    await second.initialize(ws, {
        initializationOptions: { project: { readonly: "on" } },
    });
    const [uri] = second.open("main.cpp", 0, {
        text: '#include "renamed.h"\n' + MAIN.split("\n").slice(1).join("\n"),
    });
    expect((await second.documentLinks(uri)) ?? []).toEqual([]);
    const defs = await second.definitionAt(uri, 0, 12);
    expect(defs === null || (Array.isArray(defs) && defs.length === 0)).toBe(true);
});

const OFF = { project: { readonly: "off" } };

test("preamble define hovers under pch", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("header.h", HEADER);
    ws.write("main.cpp", "#define LIMIT 10\n" + MAIN);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: OFF });

    // The define is compiled into the PCH and has no AST node; the null
    // from the worker falls back to the index card for the preamble
    // region (and only there).
    await client.openAndWait("main.cpp");
    const uri = ws.uri("main.cpp");
    const hover = await client.hoverAt(uri, 0, 9);
    expect(JSON.stringify(hover?.contents ?? "")).toContain("LIMIT");
});

test("off compiles on demand", async ({ session }) => {
    const ws = writeProject(session);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: OFF });

    // didOpen alone starts nothing (the pre-readonly contract): the
    // diagnostics push rides the first read's pulled compile.
    const uri = ws.uri("main.cpp");
    let published = false;
    const arrived = client.armDiagnostics(uri).then(() => {
        published = true;
    });
    client.open("main.cpp");
    await sleep(SETTLE_TIME);
    expect(published).toBe(false);

    const hover = await client.hoverAt(uri, 4, 11);
    expect(JSON.stringify(hover?.contents ?? "")).toContain("add");
    await arrived;
    client.assertNoErrors(uri);
});

test("index answers while pull compile runs", async ({ session }) => {
    const ws = writeProject(session);

    const first = session.spawn(ws);
    await first.initialize(ws, { initializationOptions: AUTO });
    const [warm] = first.open("main.cpp");
    expect(await first.waitForIndex(warm, "twice")).toBe(true);
    await first.shutdown();

    // off: the first read pulls the compile, but must not block on it —
    // the warm shard answers instantly, and the detached pull still
    // lands the AST (diagnostics prove it).
    const second = session.spawn(ws);
    await second.initialize(ws, { initializationOptions: OFF });
    const arrived = second.armDiagnostics(ws.uri("main.cpp"));
    const [uri] = second.open("main.cpp");
    const symbols = (await second.documentSymbols(uri)) as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("twice");
    await arrived;
    second.assertNoErrors(uri);
});
