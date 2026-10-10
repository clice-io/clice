/// Build configurations: the menu the tagged rules declare, the persisted
/// selection a restart activates, one index library per configuration,
/// and the documented example layouts of samples/layouts.

import * as fs from "node:fs";
import * as path from "node:path";
import type { Serve } from "@clice/tools/actions";
import { wireKeys, type ListConfigurationsResult } from "@clice/tools/protocol";
import type { Workspace } from "@clice/tools/workspace";
import { expect, serve } from "../../fixtures.ts";

const test = serve("layouts/two_configurations");

/// Persist `name` and start a fresh server on the workspace — what an
/// editor does to apply a switch.
async function switchAndRestart(s: Serve, name: string): Promise<void> {
    expect(await s.switchConfiguration(name)).toEqual({ success: true });
    await s.restart();
}

async function inactiveLines(s: Serve, file: string): Promise<number[]> {
    await s.compiled(file);
    return s.inactiveLines(file);
}

/// Whether the configuration's index library holds a database.
function hasLibrary(workspace: Workspace, configuration: string): boolean {
    const library = workspace.indexLibrary(configuration);
    return library !== undefined && fs.existsSync(path.join(library, "index.mdb"));
}

test("menu and selection layers", async ({ s }) => {
    const listed = await s.configurations();
    expect(listed).toEqual({
        configurations: ["debug", "release"],
        active: "debug",
        selected: "",
        defaultConfiguration: "debug",
    });
    expect(Object.keys(listed).sort()).toEqual(
        [
            ...wireKeys<ListConfigurationsResult>()([
                "active",
                "configurations",
                "defaultConfiguration",
                "selected",
            ]),
        ].sort(),
    );

    expect(await s.switchConfiguration("nope"), "a name no rule declares").toEqual({
        success: false,
    });
    expect(await s.switchConfiguration("release")).toEqual({ success: true });
    expect(JSON.parse(s.disk.read(".clice/state.json"))).toEqual({ configuration: "release" });
    // The running server keeps its configuration; only the selection moved.
    expect(await s.configurations()).toMatchObject({
        active: "debug",
        selected: "release",
    });
});

test("restart activates the selection", async ({ s }) => {
    expect(await inactiveLines(s, "main.cpp"), "the release branch is inactive").toEqual([3]);
    expect(
        (await s.errors("gated.cpp")).length,
        "RELEASE is undefined under debug",
    ).toBeGreaterThan(0);

    await switchAndRestart(s, "release");
    expect(await s.configurations()).toMatchObject({
        active: "release",
        selected: "release",
    });
    expect(await inactiveLines(s, "main.cpp"), "the debug branch is inactive").toEqual([5]);
    expect(await s.errors("gated.cpp"), "RELEASE is defined under release").toEqual([]);

    await switchAndRestart(s, "debug");
    expect(await s.configurations()).toMatchObject({
        active: "debug",
        selected: "debug",
    });
    expect(await inactiveLines(s, "main.cpp")).toEqual([3]);
});

test("each configuration keeps its own index", async ({ s }) => {
    await s.indexed();
    expect(await s.workspaceSymbols("debug_only")).toHaveLength(1);
    expect((await s.counts()).index, "the cold start indexes").toBeGreaterThan(0);
    expect(hasLibrary(s.workspace, "debug")).toBe(true);
    expect(s.workspace.indexLibrary("release")).toBeUndefined();

    await switchAndRestart(s, "release");
    await s.indexed();
    expect(await s.workspaceSymbols("release_only")).toHaveLength(1);
    expect(hasLibrary(s.workspace, "release")).toBe(true);
    expect(
        (await s.workspaceSymbols("debug_only"))?.length ?? 0,
        "the debug index is not consulted under release",
    ).toBe(0);

    // Back under debug the persisted library serves as is: the sweep's
    // hash gate finds nothing changed, so no unit is indexed again.
    await switchAndRestart(s, "debug");
    await s.indexed();
    expect(await s.workspaceSymbols("debug_only")).toHaveLength(1);
    expect((await s.counts()).index, "no unit was reindexed").toBe(0);
});

test("artifacts are keyed per configuration", async ({ s }) => {
    // shared.cpp compiles with the same command under both configurations,
    // yet each configuration builds its own PCH: the dependency stamps that
    // vouch for a PCH live in the configuration's library, so a blob one
    // configuration rebuilt must never pass the other's check.
    await s.clean("shared.cpp");
    const debugPch = s.workspace.pchFiles();
    expect(debugPch.length).toBe(1);
    const debugMtime = fs.statSync(debugPch[0]!).mtimeMs;

    await switchAndRestart(s, "release");
    await s.clean("shared.cpp");
    expect(s.workspace.pchFiles().length, "release builds a PCH of its own").toBe(2);

    await switchAndRestart(s, "debug");
    await s.clean("shared.cpp");
    expect(s.workspace.pchFiles().length, "debug reuses its PCH").toBe(2);
    expect(fs.statSync(debugPch[0]!).mtimeMs).toBe(debugMtime);
});

test("pins stay with their configuration", async ({ s }) => {
    await s.compiled("lib.h");
    const contexts = await s.contexts("lib.h");
    expect(contexts.total, "both units host the header").toBe(2);
    const other = contexts.contexts.find((c) => c.uri.endsWith("other.cpp"));
    expect(other).toBeDefined();
    expect((await s.switchContext("lib.h", s.relative(other!.uri))).success).toBe(true);
    expect((await s.currentContext("lib.h")).context?.uri).toBe(other!.uri);

    await switchAndRestart(s, "release");
    await s.compiled("lib.h");
    expect((await s.currentContext("lib.h")).automatic, "no pin under release").toBe(true);

    await switchAndRestart(s, "debug");
    await s.compiled("lib.h");
    const restored = await s.currentContext("lib.h");
    expect(restored.context?.uri).toBe(other!.uri);
    expect(restored.automatic).toBe(false);
});

test("command line overrides the selection", async ({ s }) => {
    expect(await s.switchConfiguration("release")).toEqual({ success: true });
    await s.stop();

    await s.start({ args: ["serve", "--configuration", "debug"] });
    expect(await s.configurations()).toMatchObject({
        active: "debug",
        selected: "release",
    });
    expect(
        (await s.errors("gated.cpp")).length,
        "the command line's debug is active",
    ).toBeGreaterThan(0);
    expect(
        await s.switchConfiguration("release"),
        "the command line owns a pinned session's choice",
    ).toEqual({ success: false });
    await s.stop();

    // An unknown command-line name is skipped: the selection still wins.
    await s.start({ args: ["serve", "--configuration", "nope"] });
    expect(await s.configurations()).toMatchObject({ active: "release" });
    expect(await s.switchConfiguration("debug"), "nothing pins this session").toEqual({
        success: true,
    });
});

serve("layouts/two_configurations", {
    files: { ".clice/state.json": '{"configuration": "gone"}\n' },
})("unknown selection falls back untouched", async ({ s }) => {
    expect(await s.configurations()).toMatchObject({
        active: "debug",
        selected: "gone",
    });
    expect(JSON.parse(s.disk.read(".clice/state.json"))).toEqual({ configuration: "gone" });
});

serve("layouts/single_root")("untagged rules have no menu", async ({ s }) => {
    expect(await s.configurations()).toEqual({
        configurations: [],
        active: "",
        selected: "",
        defaultConfiguration: "",
    });
    expect(await s.switchConfiguration("debug")).toEqual({ success: false });

    // A selection left behind by another rule set is ignored, not applied.
    await s.offline(() => {
        s.disk.write(".clice/state.json", '{"configuration": "debug"}\n');
    });
    expect(await s.configurations()).toMatchObject({ active: "", selected: "debug" });
});

serve("layouts/board_defaults")("default command per board", async ({ s }) => {
    expect(await s.configurations()).toMatchObject({
        configurations: ["board-a", "board-b"],
        active: "board-a",
    });
    expect(await s.errors("src/main.c"), "board a's default command defines its board").toEqual([]);
    expect(await s.errors("include/board.h")).toEqual([]);
    expect(await inactiveLines(s, "include/board.h"), "board b's branch is inactive").toEqual([5]);

    await switchAndRestart(s, "board-b");
    expect(await s.errors("src/main.c")).toEqual([]);
    expect(await inactiveLines(s, "include/board.h"), "board a's branch is inactive").toEqual([2]);
});

serve("layouts/firmware_tools")("tagged rules switch while untagged stay", async ({ s }) => {
    expect(await s.errors("firmware/main.c")).toEqual([]);
    expect(await inactiveLines(s, "firmware/main.c"), "board a's database is active").toEqual([3]);
    expect(await s.errors("tools/gen.cpp"), "the untagged rule's database serves tools/").toEqual(
        [],
    );

    await switchAndRestart(s, "board-b");
    expect(await inactiveLines(s, "firmware/main.c"), "board b's database is active").toEqual([1]);
    expect(await s.errors("tools/gen.cpp"), "the untagged rule keeps serving tools/").toEqual([]);
});
