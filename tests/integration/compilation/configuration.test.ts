/// Build configurations: the menu the tagged rules declare, the persisted
/// selection a restart activates, one index library per configuration,
/// and the three documented example layouts under tests/data/cdb.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { waitUntil, type CliceClient } from "@clice/tools/client";
import { DATA_DIR } from "@clice/tools/compile-commands";
import { wireKeys, type ListConfigurationsResult } from "@clice/tools/protocol";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test, type SessionFactory } from "../fixtures.ts";

/// Persist `name` and start a fresh server on the workspace — what an
/// editor does to apply a switch.
async function switchAndRestart(
    session: SessionFactory,
    client: CliceClient,
    workspace: Workspace,
    name: string,
): Promise<CliceClient> {
    expect(await client.switchConfiguration(name)).toEqual({ success: true });
    await client.shutdown();
    return session.spawn(workspace).initialize(workspace);
}

function runClice(...args: string[]) {
    return spawnSync(cliceExecutable(), args, {
        encoding: "utf8",
        timeout: 120_000,
        maxBuffer: 64 * 1024 * 1024,
    });
}

/// The server's log once the cold-start sweep has run its round: every
/// unit the hash gate let through has logged its `Indexing` line by then.
async function sweptLog(client: CliceClient): Promise<string> {
    await waitUntil(
        () => client.drainedStderr().toString("utf8").includes("[perf:index] phase=run "),
        { timeout: 30_000, interval: 100, description: "the indexing sweep to finish" },
    );
    return client.drainedStderr().toString("utf8");
}

/// Whether the configuration's index library holds a database.
function hasLibrary(workspace: Workspace, configuration: string): boolean {
    const library = workspace.indexLibrary(configuration);
    return library !== undefined && fs.existsSync(path.join(library, "index.mdb"));
}

test("menu and selection layers", async ({ session }) => {
    const { client, workspace } = await session("cdb/two_configurations");
    const listed = await client.listConfigurations();
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

    expect(await client.switchConfiguration("nope"), "a name no rule declares").toEqual({
        success: false,
    });
    expect(await client.switchConfiguration("release")).toEqual({ success: true });
    expect(JSON.parse(workspace.read(".clice/state.json"))).toEqual({ configuration: "release" });
    // The running server keeps its configuration; only the selection moved.
    expect(await client.listConfigurations()).toMatchObject({
        active: "debug",
        selected: "release",
    });
});

test("restart activates the selection", async ({ session }) => {
    const { client, workspace } = await session("cdb/two_configurations");
    const [main] = await client.openAndWait("main.cpp");
    expect(await client.inactiveLines(main), "the release branch is inactive").toEqual([3]);
    const [gated] = await client.openAndWait("gated.cpp");
    client.assertHasErrors(gated, "RELEASE is undefined under debug");

    const release = await switchAndRestart(session, client, workspace, "release");
    expect(await release.listConfigurations()).toMatchObject({
        active: "release",
        selected: "release",
    });
    const [main2] = await release.openAndWait("main.cpp");
    expect(await release.inactiveLines(main2), "the debug branch is inactive").toEqual([5]);
    const [gated2] = await release.openAndWait("gated.cpp");
    release.assertNoErrors(gated2, "RELEASE is defined under release");

    const debug = await switchAndRestart(session, release, workspace, "debug");
    expect(await debug.listConfigurations()).toMatchObject({ active: "debug", selected: "debug" });
    const [main3] = await debug.openAndWait("main.cpp");
    expect(await debug.inactiveLines(main3)).toEqual([3]);
});

test("each configuration keeps its own index", async ({ session }) => {
    const { client, workspace } = await session("cdb/two_configurations");
    const [main] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(main, "debug_only")).toBe(true);
    expect(await sweptLog(client), "the cold start indexes").toContain("] Indexing ");
    expect(hasLibrary(workspace, "debug")).toBe(true);
    expect(workspace.indexLibrary("release")).toBeUndefined();

    const release = await switchAndRestart(session, client, workspace, "release");
    const [main2] = await release.openAndWait("main.cpp");
    expect(await release.waitForIndex(main2, "release_only")).toBe(true);
    expect(hasLibrary(workspace, "release")).toBe(true);
    expect(
        (await release.workspaceSymbols("debug_only"))?.length ?? 0,
        "the debug index is not consulted under release",
    ).toBe(0);

    // Back under debug the persisted library serves as is: the sweep's
    // hash gate finds nothing changed, so no unit is indexed again.
    const debug = await switchAndRestart(session, release, workspace, "debug");
    const [main3] = await debug.openAndWait("main.cpp");
    expect(await debug.waitForIndex(main3, "debug_only")).toBe(true);
    const log = await sweptLog(debug);
    expect(log, "no unit was reindexed").not.toContain("] Indexing ");
    expect(log).not.toContain("reindexing");
});

test("artifacts are keyed per configuration", async ({ session }) => {
    // shared.cpp compiles with the same command under both configurations,
    // yet each configuration builds its own PCH: the dependency stamps that
    // vouch for a PCH live in the configuration's library, so a blob one
    // configuration rebuilt must never pass the other's check.
    const { client, workspace } = await session("cdb/two_configurations");
    const [shared] = await client.openAndWait("shared.cpp");
    client.assertCleanCompile(shared);
    const debugPch = workspace.pchFiles();
    expect(debugPch.length).toBe(1);
    const debugMtime = fs.statSync(debugPch[0]!).mtimeMs;

    const release = await switchAndRestart(session, client, workspace, "release");
    const [shared2] = await release.openAndWait("shared.cpp");
    release.assertCleanCompile(shared2);
    expect(workspace.pchFiles().length, "release builds a PCH of its own").toBe(2);

    const debug = await switchAndRestart(session, release, workspace, "debug");
    const [shared3] = await debug.openAndWait("shared.cpp");
    debug.assertCleanCompile(shared3);
    expect(workspace.pchFiles().length, "debug reuses its PCH").toBe(2);
    expect(fs.statSync(debugPch[0]!).mtimeMs).toBe(debugMtime);
});

test("pins stay with their configuration", async ({ session }) => {
    const { client, workspace } = await session("cdb/two_configurations");
    const header = workspace.uri("lib.h");
    await client.openAndWait("lib.h");
    const contexts = await client.queryContext(header);
    expect(contexts.total, "both units host the header").toBe(2);
    const other = contexts.contexts.find((c) => c.uri.endsWith("other.cpp"));
    expect(other).toBeDefined();
    expect((await client.switchContext(header, other!.uri)).success).toBe(true);
    expect((await client.currentContext(header)).context?.uri).toBe(other!.uri);

    const release = await switchAndRestart(session, client, workspace, "release");
    await release.openAndWait("lib.h");
    expect((await release.currentContext(header)).context, "no pin under release").toBeNull();

    const debug = await switchAndRestart(session, release, workspace, "debug");
    await debug.openAndWait("lib.h");
    expect((await debug.currentContext(header)).context?.uri).toBe(other!.uri);
});

test("command line overrides the selection", async ({ session }) => {
    const { client, workspace } = await session("cdb/two_configurations");
    expect(await client.switchConfiguration("release")).toEqual({ success: true });
    await client.shutdown();

    const pinned = await session
        .spawn(workspace, { args: ["serve", "--configuration", "debug"] })
        .initialize(workspace);
    expect(await pinned.listConfigurations()).toMatchObject({
        active: "debug",
        selected: "release",
    });
    const [gated] = await pinned.openAndWait("gated.cpp");
    pinned.assertHasErrors(gated, "the command line's debug is active");
    expect(
        await pinned.switchConfiguration("release"),
        "the command line owns a pinned session's choice",
    ).toEqual({ success: false });
    await pinned.shutdown();

    // An unknown command-line name is skipped: the selection still wins.
    const unknown = await session
        .spawn(workspace, { args: ["serve", "--configuration", "nope"] })
        .initialize(workspace);
    expect(await unknown.listConfigurations()).toMatchObject({ active: "release" });
});

test("unknown selection falls back untouched", async ({ session }) => {
    const { client, workspace } = await session("cdb/two_configurations");
    await client.shutdown();
    workspace.write(".clice/state.json", '{"configuration": "gone"}\n');

    const restarted = await session.spawn(workspace).initialize(workspace);
    expect(await restarted.listConfigurations()).toMatchObject({
        active: "debug",
        selected: "gone",
    });
    expect(JSON.parse(workspace.read(".clice/state.json"))).toEqual({ configuration: "gone" });
});

test("batch index per configuration", ({ session }) => {
    const workspace = session.tmpdir();
    fs.cpSync(path.join(DATA_DIR, "cdb", "two_configurations"), workspace.root, {
        recursive: true,
    });
    const release = ["--workspace", workspace.root, "--configuration", "release"];

    const indexed = runClice("index", ...release, "--workers", "2");
    expect(indexed.status, `stderr: ${indexed.stderr}`).toBe(0);
    expect(indexed.stdout).toContain("Indexed 4 translation units in");
    expect(hasLibrary(workspace, "release")).toBe(true);

    const stats = runClice("index", "--stats", ...release);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Configuration: release");
    expect(stats.stdout).toContain("Translation units: 4");

    // The default configuration's library was never written.
    const missing = runClice("index", "--stats", "--workspace", workspace.root);
    expect(missing.status).toBe(1);
    expect(missing.stderr).toContain("No index cache");

    const debug = runClice("index", "--workspace", workspace.root, "--workers", "2");
    expect(debug.status, `stderr: ${debug.stderr}`).toBe(0);
    expect(debug.stdout).toContain("Indexed 4 translation units in");
    expect(hasLibrary(workspace, "debug")).toBe(true);
    const both = runClice("index", "--stats", "--workspace", workspace.root);
    expect(both.stdout).toContain("Configuration: debug");
    expect(both.stdout).toContain("Translation units: 4");
});

test("scripted commands reject unknown names", ({ session }) => {
    // The server falls back so an editor always starts; a batch or
    // inspection run with a misspelt name must fail, not report success
    // for another configuration.
    const workspace = session.tmpdir();
    fs.cpSync(path.join(DATA_DIR, "cdb", "two_configurations"), workspace.root, {
        recursive: true,
    });
    const unknown = ["--workspace", workspace.root, "--configuration", "nope"];
    for (const [command, status] of [
        [["index", "--workers", "2"], 1],
        [["index", "--stats"], 1],
        [["lint", "--workers", "2"], 2],
    ] as const) {
        const run = runClice(...command, ...unknown);
        expect(run.status, command.join(" ")).toBe(status);
        expect(run.stderr, command.join(" ")).toContain("names no rule's configuration");
    }
    const gated = path.join(workspace.root, "gated.cpp");
    expect(runClice("inspect", "--configuration", "nope", "hover", gated).status).toBe(1);
    const inspect = (...args: string[]) => {
        const run = runClice("inspect", ...args, "hover", gated);
        expect(run.status, `stderr: ${run.stderr}`).toBe(0);
        const output = JSON.parse(run.stdout) as {
            files: Record<string, { diagnostics?: string[] | null }>;
        };
        return Object.values(output.files).flatMap((file) => file.diagnostics ?? []);
    };
    expect(inspect(), "the default configuration lacks RELEASE").toEqual(
        expect.arrayContaining([expect.stringContaining("missing RELEASE")]),
    );
    expect(inspect("--configuration", "release")).toEqual([]);
});

test("untagged rules have no menu", async ({ session }) => {
    const { client, workspace } = await session("cdb/single_root");
    expect(await client.listConfigurations()).toEqual({
        configurations: [],
        active: "",
        selected: "",
        defaultConfiguration: "",
    });
    expect(await client.switchConfiguration("debug")).toEqual({ success: false });
    await client.shutdown();

    // A selection left behind by another rule set is ignored, not applied.
    workspace.write(".clice/state.json", '{"configuration": "debug"}\n');
    const restarted = await session.spawn(workspace).initialize(workspace);
    expect(await restarted.listConfigurations()).toMatchObject({ active: "", selected: "debug" });
});

test("default command per board", async ({ session }) => {
    const { client, workspace } = await session("cdb/board_defaults");
    expect(await client.listConfigurations()).toMatchObject({
        configurations: ["board-a", "board-b"],
        active: "board-a",
    });
    const [main] = await client.openAndWait("src/main.c");
    client.assertNoErrors(main, "board a's default command defines its board");
    const [header] = await client.openAndWait("include/board.h");
    client.assertNoErrors(header);
    expect(await client.inactiveLines(header), "board b's branch is inactive").toEqual([5]);

    const b = await switchAndRestart(session, client, workspace, "board-b");
    const [main2] = await b.openAndWait("src/main.c");
    b.assertNoErrors(main2);
    const [header2] = await b.openAndWait("include/board.h");
    expect(await b.inactiveLines(header2), "board a's branch is inactive").toEqual([2]);
});

test("tagged rules switch while untagged stay", async ({ session }) => {
    const { client, workspace } = await session("cdb/firmware_tools");
    const [main] = await client.openAndWait("firmware/main.c");
    client.assertNoErrors(main);
    expect(await client.inactiveLines(main), "board a's database is active").toEqual([3]);
    const [tool] = await client.openAndWait("tools/gen.cpp");
    client.assertNoErrors(tool, "the untagged rule's database serves tools/");

    const b = await switchAndRestart(session, client, workspace, "board-b");
    const [main2] = await b.openAndWait("firmware/main.c");
    expect(await b.inactiveLines(main2), "board b's database is active").toEqual([1]);
    const [tool2] = await b.openAndWait("tools/gen.cpp");
    b.assertNoErrors(tool2, "the untagged rule keeps serving tools/");
});
