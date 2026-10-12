import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

// Every case marks the declaration main.cpp uses, in a header main.cpp
// includes or in a module it imports, directly or through a header the
// module includes. What a change while the server was down invalidates is
// built again on restart, once; the rest is reused.

/// The builds a change to the case's inputs costs: a module interface's
/// PCM where the case has one, else main.cpp's PCH.
function rebuilt(t: TestContext): { pch: number; pcm: number } {
    return t.disk.list("*.cppm").length > 0 ? { pch: 0, pcm: 1 } : { pch: 1, pcm: 0 };
}

async function builds(s: Serve): Promise<{ pch: number; pcm: number }> {
    await s.sync();
    const { pch, pcm } = await s.counts();
    return { pch, pcm };
}

test("cache survives a restart", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    const links = await s.documentLinks("main.cpp");
    await s.restart();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual({ pch: 0, pcm: 0 });
    // The links of a reused preamble come from the cache with it.
    expect(await s.documentLinks("main.cpp")).toEqual(links);
    await t.show("links", s.documentLinks("main.cpp"));
});

test("offline edit reaches its importers", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    const declaration = t.marker("declaration");
    await s.offline(() => {
        t.disk.edit(declaration.path, {
            replace: "area(const Circle& circle)",
            with: "area(const Circle& circle, int scale)",
        });
    });
    await t.show("errors", s.errors("main.cpp"));
    expect(await builds(s)).toEqual(rebuilt(t));
});

test("offline flag change rebuilds", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    await s.offline(() => {
        const units = t.disk.list("*.{cpp,cppm}").map((file) => [file, ["-DREVISION=2"]]);
        t.disk.database(Object.fromEntries(units) as Record<string, string[]>);
    });
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual(rebuilt(t));
});

test("offline touch reuses", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    // A checkout there and back rewrites the same bytes at a new time.
    const declaration = t.marker("declaration");
    await s.offline(() => {
        t.disk.write(declaration.path, t.disk.read(declaration.path));
        t.disk.touch(declaration.path, new Date(Date.now() + 5_000));
    });
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    expect(await builds(s)).toEqual({ pch: 0, pcm: 0 });
});
