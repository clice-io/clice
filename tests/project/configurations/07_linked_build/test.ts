import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

/// The index databases the cache holds, by the label before their hash.
function libraries(t: TestContext): string[] {
    return t.disk
        .list(".clice/cache/*/index/*/index.mdb")
        .map((file) => file.split("/").at(-2)?.split("~")[0] ?? file);
}

/// Point out/current at another build, as the build system does, and have
/// the server look at its databases.
async function flip(t: TestContext, s: Serve, build: string): Promise<void> {
    t.disk.linkDirectory("out/current", `out/${build}`);
    await s.poll("cdb");
    await s.indexed();
}

test("the link picks the build", async (t) => {
    t.disk.linkDirectory("out/current", "out/release");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("release", {
        symbols: s.workspaceSymbols("_only"),
        inactive: s.inactiveLines("main.cpp"),
    });
    expect(libraries(t)).toEqual(["active-out_release"]);
    // out/debug is the debug configuration's database too: the build
    // shares that configuration's index.
    await flip(t, s, "debug");
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("debug", {
        menu: s.configurations(),
        symbols: s.workspaceSymbols("_only"),
        inactive: s.inactiveLines("main.cpp"),
    });
    expect(libraries(t)).toEqual(["active-out_release", "debug"]);
});

test("switching back indexes nothing", async (t) => {
    t.disk.linkDirectory("out/current", "out/release");
    const s = await t.start();
    await s.indexed();
    await flip(t, s, "debug");
    const indexed = (await s.counts()).index;
    await flip(t, s, "release");
    await t.show("release again", s.workspaceSymbols("_only"));
    expect((await s.counts()).index).toBe(indexed);
});

test("a server starts on the build the link names", async (t) => {
    t.disk.linkDirectory("out/current", "out/release");
    const s = await t.start();
    await s.indexed();
    await s.offline(() => {
        t.disk.linkDirectory("out/current", "out/debug");
    });
    await s.indexed();
    await t.show("debug", s.workspaceSymbols("_only"));
    expect(libraries(t)).toEqual(["active-out_release", "debug"]);
});

test("the hand-written configuration reuses the index", async (t) => {
    t.disk.linkDirectory("out/current", "out/debug");
    const s = await t.start();
    await s.indexed();
    expect(libraries(t)).toEqual(["debug"]);
    expect(await s.switchConfiguration("debug")).toEqual({ success: true });
    await s.restart();
    await s.indexed();
    await t.show("debug", { menu: s.configurations(), symbols: s.workspaceSymbols("_only") });
    expect((await s.counts()).index).toBe(0);
});
