import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

/// Persist `name` and start the server again: what an editor does to apply
/// a switch.
async function switchTo(s: Serve, name: string): Promise<void> {
    expect(await s.switchConfiguration(name)).toEqual({ success: true });
    await s.restart();
}

/// The configurations whose index library the cache holds.
function libraries(t: TestContext): string[] {
    return t.disk
        .list(".clice/cache/*/index/*/index.mdb")
        .map((file) => file.split("/").at(-2)?.split("~")[0] ?? file);
}

test("menu and selection", async (t) => {
    const s = await t.start();
    await t.show("start", s.configurations());
    expect(Object.keys(await s.client.listConfigurations()).sort()).toEqual([
        "active",
        "configurations",
        "defaultConfiguration",
        "selected",
    ]);
    // A name no rule declares.
    expect(await s.switchConfiguration("nope")).toEqual({ success: false });
    expect(await s.switchConfiguration("release")).toEqual({ success: true });
    await t.show("state", t.disk.read(".clice/state.json"));
    // The running server keeps its configuration; only the selection moved.
    await t.show("switched", s.configurations());
});

test("restart activates the selection", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("debug", { main: s.inactiveLines("main.cpp"), gated: s.errors("gated.cpp") });
    // After a restart main.cpp compiles before its lines are asked: an
    // answer from its shard, which the background index may write first,
    // carries no inactive lines until the compile refreshes it.
    await switchTo(s, "release");
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("release", { menu: s.configurations(), main: s.inactiveLines("main.cpp") });
    expect(await s.errors("gated.cpp")).toEqual([]);
    await switchTo(s, "debug");
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("debug again", { menu: s.configurations(), main: s.inactiveLines("main.cpp") });
});

test("each configuration keeps its own index", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("debug", s.workspaceSymbols("_only"));
    expect(libraries(t)).toEqual(["debug"]);
    await switchTo(s, "release");
    await s.indexed();
    await t.show("release", s.workspaceSymbols("_only"));
    expect(libraries(t)).toEqual(["debug", "release"]);
    // Back under debug its library serves as it was: nothing is indexed again.
    await switchTo(s, "debug");
    await s.indexed();
    await t.show("debug again", s.workspaceSymbols("_only"));
    expect((await s.counts()).index).toBe(0);
});

test("artifacts are keyed per configuration", async (t) => {
    // shared.cpp compiles with one command under both configurations, yet
    // each builds its own PCH: the stamps that vouch for a PCH live in the
    // configuration's library, so a PCH one configuration rebuilt must
    // never pass the other's check.
    const s = await t.start();
    expect(await s.diagnostics("shared.cpp")).toEqual([]);
    expect((await s.counts()).pch).toBe(1);
    await switchTo(s, "release");
    expect(await s.diagnostics("shared.cpp")).toEqual([]);
    expect((await s.counts()).pch).toBe(1);
    await switchTo(s, "debug");
    expect(await s.diagnostics("shared.cpp")).toEqual([]);
    expect((await s.counts()).pch).toBe(0);
});

test("pins stay with their configuration", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("lib.h")).toEqual([]);
    await t.show("hosts", s.contexts("lib.h"));
    expect((await s.switchContext("lib.h", "other.cpp")).success).toBe(true);
    await t.show("pinned", s.currentContext("lib.h"));
    await switchTo(s, "release");
    await t.show("release", s.currentContext("lib.h"));
    await switchTo(s, "debug");
    await t.show("debug again", s.currentContext("lib.h"));
});

test("command line overrides the selection", async (t) => {
    const s = await t.start();
    expect(await s.switchConfiguration("release")).toEqual({ success: true });
    await s.stop();

    const pinned = await t.start({ args: ["serve", "--configuration", "debug"] });
    await t.show("pinned", { menu: pinned.configurations(), gated: pinned.errors("gated.cpp") });
    // The command line owns a pinned session's choice.
    expect(await pinned.switchConfiguration("release")).toEqual({ success: false });
    await pinned.stop();

    // An unknown name on the command line is skipped: the selection wins.
    const unknown = await t.start({ args: ["serve", "--configuration", "nope"] });
    await t.show("unknown", unknown.configurations());
    expect(await unknown.switchConfiguration("debug")).toEqual({ success: true });
});

test("unknown selection falls back untouched", async (t) => {
    const s = await t.start();
    await s.offline(() => {
        t.disk.write(".clice/state.json", '{"configuration": "gone"}\n');
    });
    await t.show("menu", s.configurations());
    await t.show("state", t.disk.read(".clice/state.json"));
});

test("batch index per configuration", async (t) => {
    await t.cli("index --configuration release --workers 2");
    expect(libraries(t)).toEqual(["release"]);
    // The default configuration's library was never written.
    await t.cli("index --stats");
    await t.cli("index --workers 2");
    expect(libraries(t)).toEqual(["debug", "release"]);
});

test(
    "scripted commands reject unknown names",
    { errors: ["--configuration nope names no rule's configuration"] },
    async (t) => {
        // The server falls back so an editor always starts; a batch or
        // inspection run with a misspelt name fails instead of reporting on
        // another configuration.
        await t.cli("index --configuration nope --workers 2");
        await t.cli("index --stats --configuration nope");
        await t.cli("lint --configuration nope --workers 2");
        await t.cli("inspect --configuration nope hover gated.cpp");
        // --flags replaces the rules the name would select among.
        await t.cli([
            "inspect",
            "--configuration",
            "release",
            "--flags",
            '["clang++"]',
            "hover",
            "gated.cpp",
        ]);
        await t.cli("inspect hover gated.cpp");
        await t.cli("inspect --configuration release hover gated.cpp");
    },
);
