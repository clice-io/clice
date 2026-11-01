import { expect } from "vitest";
import type { Serve } from "@clice/tools/testing";
import { test } from "@clice/tools/testing";

// The pool keeps one AST: compiling b.cpp evicts a.cpp's. Back at a.cpp,
// what its compile's AST answered answers again, and its index answers its
// navigation, with no compile and no refresh: nothing it read changed. The
// rest compiles it once more.

const KEEP_ONE = { project: { enable_indexing: false, max_resident_documents: 1 } };
const REFRESH = "workspace/semanticTokens/refresh";
const REFRESHES = { workspace: { semanticTokens: { refreshSupport: true } } };

async function compiles(s: Serve, file: string): Promise<number> {
    await s.sync();
    return (await s.counts()).files[file]?.compile ?? 0;
}

test("its answers come back", async (t) => {
    const s = await t.start({ config: KEEP_ONE, capabilities: REFRESHES });
    const first = t.at("a.cpp", "int NAME = add(1, 2);");
    const asked = () =>
        Promise.all([
            s.semanticTokens("a.cpp"),
            s.foldingRanges("a.cpp"),
            s.documentSymbols("a.cpp"),
            s.documentLinks("a.cpp"),
            s.inlayHints(first),
        ]);
    // The actions ask the whole document's tokens; an editor asks a range
    // of them too, which the same kept tokens answer.
    const ranged = () =>
        s.client.sendRequest("textDocument/semanticTokens/range", {
            textDocument: { uri: s.uri("a.cpp") },
            range: { start: { line: 2, character: 0 }, end: { line: 3, character: 0 } },
        });
    await s.diagnostics("a.cpp");
    const compiled = await asked();
    const compiledRange = await ranged();
    await s.diagnostics("b.cpp");
    const refreshes = await s.serverRequests(REFRESH);
    expect(await asked()).toEqual(compiled);
    expect(await ranged()).toEqual(compiledRange);
    await t.show("evicted", {
        tokens: s.semanticTokens("a.cpp"),
        outline: s.documentSymbols("a.cpp"),
        hints: s.inlayHints(first),
        definition: s.definition(t.at("a.cpp", "int NAME = |add(1, 2);")),
    });
    expect(await compiles(s, "a.cpp")).toBe(1);
    expect(await s.serverRequests(REFRESH)).toBe(refreshes);
});

test("the rest compiles it once", async (t) => {
    const s = await t.start({ config: KEEP_ONE });
    await s.diagnostics("a.cpp");
    await s.inlayHints(t.at("a.cpp", "int NAME = add(1, 2);"));
    await s.diagnostics("b.cpp");
    await t.show("another range", s.inlayHints(t.at("a.cpp", "int other = add(3, 4);")));
    await t.show("hover", s.hover(t.at("a.cpp", "int NAME = |add(1, 2);")));
    expect(await compiles(s, "a.cpp")).toBe(2);
});

test("an edit drops them", async (t) => {
    const s = await t.start({ config: KEEP_ONE });
    await s.diagnostics("a.cpp");
    await s.documentSymbols("a.cpp");
    await s.diagnostics("b.cpp");
    s.edit("a.cpp", { after: "int other = add(3, 4);\n", insert: "int third = 5;\n" });
    await t.show("outline", s.documentSymbols("a.cpp"));
    expect(await compiles(s, "a.cpp")).toBe(2);
});

test("any request is a use", async (t) => {
    // Two kept. The index answers a.cpp's definition, and keeps a.cpp over
    // b.cpp when c.cpp compiles.
    const s = await t.start({ config: { project: { max_resident_documents: 2 } } });
    await s.diagnostics("a.cpp");
    await s.diagnostics("b.cpp");
    await s.definition(t.at("a.cpp", "int NAME = |add(1, 2);"));
    await s.diagnostics("c.cpp");
    await s.hover(t.at("a.cpp", "int NAME = |add(1, 2);"));
    expect(await compiles(s, "a.cpp")).toBe(1);
    await s.hover(t.at("b.cpp", "int |beta"));
    expect(await compiles(s, "b.cpp")).toBe(2);
});

test("a header changed while it was out shows", async (t) => {
    // Nothing looks at the disk but the requests: no polling, no index.
    const s = await t.start({
        config: { ...KEEP_ONE, tracker: { workspace_poll_seconds: 0 } },
    });
    await s.diagnostics("a.cpp");
    await s.documentSymbols("a.cpp");
    await s.diagnostics("b.cpp");
    const written = t.disk.mtime("header.h");
    t.disk.write("header.h", "#define NAME second\nint add(int left, int right);\n");
    t.disk.touch("header.h", new Date(written.getTime() + 10_000));
    await t.show("outline", s.documentSymbols("a.cpp"));
});

test("its recompile publishes nothing new", async (t) => {
    const s = await t.start({
        config: KEEP_ONE,
        capabilities: {
            ...REFRESHES,
            textDocument: { publishDiagnostics: { relatedInformation: true } },
        },
    });
    s.open("a.cpp");
    await s.sync();
    s.open("b.cpp");
    await s.sync();
    const pushes = (await s.pushes("a.cpp")).length;
    const refreshes = await s.serverRequests(REFRESH);
    await s.hover(t.at("a.cpp", "int NAME = |add(1, 2);"));
    expect(await compiles(s, "a.cpp")).toBe(2);
    expect((await s.pushes("a.cpp")).length).toBe(pushes);
    expect(await s.serverRequests(REFRESH)).toBe(refreshes);
});

test("a crashed query keeps nothing", async (t) => {
    // The crash answers empty, and the worker that died held the AST: after
    // the save that lets the hints retry, a worker is asked again.
    const s = await t.start({
        config: KEEP_ONE,
        crash: { request: "inlayHint", file: "a.cpp" },
    });
    const first = t.at("a.cpp", "int NAME = add(1, 2);");
    await s.diagnostics("a.cpp");
    await s.inlayHints(first);
    s.save("a.cpp");
    await s.inlayHints(first);
    expect(await s.crashes()).toBe(2);
});

test("a pull's compile stays through another document's", async (t) => {
    const s = await t.start({ config: KEEP_ONE });
    const park = await s.park("compile", "a.cpp", { at: "worker" });
    const pulled = s.diagnostics("a.cpp");
    await park.reached();
    await s.diagnostics("b.cpp");
    await park.release();
    await t.show("diagnostics", pulled);
    expect(await compiles(s, "a.cpp")).toBe(1);
});
