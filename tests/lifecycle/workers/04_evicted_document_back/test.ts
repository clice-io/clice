import { expect } from "vitest";
import type { Serve } from "@clice/tools/testing";
import { test } from "@clice/tools/testing";

// The pool keeps one AST: compiling b.cpp evicts a.cpp's. Coming back to
// a.cpp is opening it again: what the index answers, its own index answers
// at once while it compiles, the rest waits for the compile.

const ONE = { env: { CLICE_TEST_MAX_RESIDENT: "1" } };
const NO_INDEX = { project: { enable_indexing: false } };
const REFRESH = "workspace/semanticTokens/refresh";

async function evict(s: Serve): Promise<void> {
    await s.diagnostics("a.cpp");
    await s.diagnostics("b.cpp");
}

async function compiles(s: Serve, file: string): Promise<number> {
    await s.sync();
    return (await s.counts()).files[file]?.compile ?? 0;
}

test("its own index answers, then the client is refreshed", async (t) => {
    const s = await t.start({
        ...ONE,
        config: NO_INDEX,
        capabilities: { workspace: { semanticTokens: { refreshSupport: true } } },
    });
    await evict(s);
    const refreshes = await s.serverRequests(REFRESH);
    await t.show("tokens", s.semanticTokens("a.cpp"));
    expect(await compiles(s, "a.cpp")).toBe(2);
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
});

test("inlay hints wait for the compile", async (t) => {
    const s = await t.start({ ...ONE, config: NO_INDEX });
    await evict(s);
    await t.show("hints", s.inlayHints("a.cpp"));
    expect(await compiles(s, "a.cpp")).toBe(2);
});

test("the same diagnostics are not published again", async (t) => {
    const s = await t.start({
        ...ONE,
        config: NO_INDEX,
        capabilities: { textDocument: { publishDiagnostics: { relatedInformation: true } } },
    });
    s.open("a.cpp");
    await s.sync();
    s.open("b.cpp");
    await s.sync();
    const pushes = (await s.pushes("a.cpp")).length;
    await s.inlayHints("a.cpp");
    expect(await compiles(s, "a.cpp")).toBe(2);
    expect((await s.pushes("a.cpp")).length).toBe(pushes);
});

test("a header changed while it was out shows", async (t) => {
    // Nothing looks at the disk but the compile: no polling, no index.
    const s = await t.start({
        ...ONE,
        config: { ...NO_INDEX, tracker: { workspace_poll_seconds: 0 } },
    });
    await evict(s);
    const written = t.disk.mtime("header.h");
    t.disk.write("header.h", "#define NAME second\nint add(int left, int right);\n");
    t.disk.touch("header.h", new Date(written.getTime() + 10_000));
    await s.documentSymbols("a.cpp");
    await s.sync();
    await t.show("outline", s.documentSymbols("a.cpp"));
});

test("a pull's compile stays through another document's", async (t) => {
    const s = await t.start({ ...ONE, config: NO_INDEX });
    const park = await s.park("compile", "a.cpp", { at: "worker" });
    const pulled = s.diagnostics("a.cpp");
    await park.reached();
    await s.diagnostics("b.cpp");
    await park.release();
    await t.show("diagnostics", pulled);
    expect(await compiles(s, "a.cpp")).toBe(1);
});
