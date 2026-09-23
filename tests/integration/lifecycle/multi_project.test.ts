/// Several workspace folders served by one server: each folder is a project
/// with its own compilation database and cache, files are routed to the
/// project that compiles them, and folders come and go at runtime.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import { SETTLE_TIME, waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

/// How long background indexing of these few-file folders may take.
const INDEX_TIMEOUT = 30_000;

/// Wait until a background index serves a definition of `name`; open
/// documents are served from their AST instead, so the files stay closed.
async function waitForDefinitionOf(client: CliceClient, name: string, uri?: string) {
    await waitUntil(
        async () =>
            ((await client.workspaceSymbols(name)) ?? []).some(
                (symbol) => symbol.name === name && (!uri || symbol.location.uri === uri),
            ),
        { timeout: INDEX_TIMEOUT, interval: SETTLE_TIME, description: `${name} indexed` },
    );
}

/// A source that compiles only with `-D<flag>`, defining `name`.
function gated(flag: string, name: string): string {
    return `#ifndef ${flag}\n#error missing ${flag}\n#endif\nint ${name}() { return 0; }\n`;
}

/// Two folders, each with a database that passes its own flag.
function twoProjects(ws: Workspace): void {
    ws.write("alpha/main.cpp", gated("IN_ALPHA", "alpha_fn"));
    ws.write("beta/main.cpp", gated("IN_BETA", "beta_fn"));
    ws.writeCDB(["alpha/main.cpp"], {
        extraArgs: ["-DIN_ALPHA"],
        at: "alpha/compile_commands.json",
    });
    ws.writeCDB(["beta/main.cpp"], { extraArgs: ["-DIN_BETA"], at: "beta/compile_commands.json" });
}

/// A library and an application including its header, each its own folder.
function libraryAndApp(ws: Workspace): void {
    ws.write(
        "lib/include/lib.h",
        "#pragma once\nint lib_fn();\ninline int shared_fn() { return 2; }\n",
    );
    ws.write("lib/src/lib.cpp", '#include "lib.h"\nint lib_fn() { return shared_fn(); }\n');
    ws.write("app/main.cpp", '#include "lib.h"\nint main() { return lib_fn(); }\n');
    const include = `-I${ws.path("lib/include")}`;
    ws.writeCDB(["lib/src/lib.cpp"], { extraArgs: [include], at: "lib/compile_commands.json" });
    ws.writeCDB(["app/main.cpp"], { extraArgs: [include], at: "app/compile_commands.json" });
}

test("folders compile separately", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertNoErrors(alpha, "alpha compiles with its own database");
    client.assertNoErrors(beta, "beta compiles with its own database");

    const stats = await client.stats();
    expect(stats.sessions, "the gauges add every folder up").toBe(2);
    expect((await client.poll("cdb")).events, "a tick finds nothing changed").toBe(0);
});

test("root uri alone serves its folder", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: null });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    client.assertNoErrors(alpha, "the root's project finds the database below it");
});

test("workspace symbol spans folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "alpha_fn"), "alpha's symbol").toBe(true);
    expect(await client.waitForIndex(alpha, "beta_fn"), "beta's symbol").toBe(true);
});

test("shared header symbol listed once", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    await client.initialize(workspace, { folders: ["app", "lib"] });

    await waitForDefinitionOf(client, "main");
    await waitForDefinitionOf(client, "lib_fn", workspace.uri("lib/src/lib.cpp"));
    const symbols = (await client.workspaceSymbols("shared_fn")) ?? [];
    expect(symbols.map((symbol) => symbol.name)).toEqual(["shared_fn"]);
});

test("each folder keeps its own cache", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "beta_fn")).toBe(true);
    await client.shutdown();
    // The client's cache directory went to the first folder; the second
    // kept its default.
    expect(fs.existsSync(workspace.path(".clice"))).toBe(true);
    expect(fs.existsSync(workspace.path("beta/.clice"))).toBe(true);
});

test("configured cache directory serves once", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    workspace.write("gamma/main.cpp", "int gamma_fn() { return 0; }\n");
    workspace.writeCDB(["gamma/main.cpp"], { at: "gamma/compile_commands.json" });
    const shared = `[project]\ncache_dir = "${workspace.path("shared").replaceAll("\\", "/")}"\n`;
    for (const folder of ["alpha", "beta", "gamma"]) {
        workspace.write(`${folder}/clice.toml`, shared);
    }
    await client.initialize(workspace, { folders: ["alpha", "beta", "gamma"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "gamma_fn")).toBe(true);
    await client.shutdown();
    // alpha takes the client's directory and beta its clice.toml's; gamma
    // finds both taken and falls back to its default.
    expect(fs.existsSync(workspace.path(".clice"))).toBe(true);
    expect(fs.existsSync(workspace.path("shared"))).toBe(true);
    expect(fs.existsSync(workspace.path("gamma/.clice"))).toBe(true);
});

test("unclaimed file opens its project", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha"] });

    // Outside every folder, with a database above it: that folder is
    // served as if it were open.
    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertNoErrors(beta, "the project found above the file compiles it");
});

test("rootless server finds projects", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: [] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertNoErrors(alpha);
    client.assertNoErrors(beta);
});

test("folder change before initialized", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, {
        folders: ["alpha"],
        beforeInitialized: () =>
            client.changeWorkspaceFolders({ added: ["beta"], removed: ["alpha"] }),
    });

    await waitForDefinitionOf(client, "beta_fn");
    expect((await client.workspaceSymbols("alpha_fn")) ?? []).toEqual([]);
});

test("added folder adopts its files", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    workspace.rm("beta/compile_commands.json");
    await client.initialize(workspace, { folders: ["alpha"] });

    // Nothing above it knows the file: the first project serves it, without
    // beta's flags.
    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertHasErrors(beta, "no project knows beta's flags yet");

    workspace.writeCDB(["beta/main.cpp"], {
        extraArgs: ["-DIN_BETA"],
        at: "beta/compile_commands.json",
    });
    await client.changeWorkspaceFolders({ added: ["beta"] });
    await client.waitForRecompile(beta);
    client.assertNoErrors(beta, "the new folder's project compiles the open file");
});

test("removed folder releases its files", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertNoErrors(beta);

    await client.changeWorkspaceFolders({ removed: ["beta"] });
    await client.waitForRecompile(beta);
    client.assertHasErrors(beta, "the remaining project has no command for it");

    await client.changeWorkspaceFolders({ added: ["beta"] });
    await client.waitForRecompile(beta);
    client.assertNoErrors(beta, "the re-added folder serves it again");
});

test("folder re-added at once keeps its cache", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });
    await waitForDefinitionOf(client, "beta_fn");

    // The control endpoint record appears only while a project holds the
    // cache directory's writer lock.
    const record = workspace.path("beta/.clice/server.json");
    const endpoint = () => (fs.existsSync(record) ? fs.readFileSync(record, "utf8") : null);
    const before = endpoint();
    expect(before).not.toBeNull();

    await client.changeWorkspaceFolders({ removed: ["beta"] });
    await client.changeWorkspaceFolders({ added: ["beta"] });
    await waitUntil(
        () => {
            const now = endpoint();
            return now !== null && now !== before;
        },
        {
            timeout: INDEX_TIMEOUT,
            interval: SETTLE_TIME,
            description: "the re-added folder to take its cache directory back",
        },
    );
});

test("removed first folder hands over", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    const [beta] = await client.openAndWait("beta/main.cpp");

    await client.changeWorkspaceFolders({ removed: ["alpha"] });
    await client.waitForRecompile(alpha);
    client.assertHasErrors(alpha, "beta serves the file, without alpha's flags");
    expect(await client.hoverAt(beta, 3, 4), "beta keeps serving its own").not.toBeNull();
});

test("removed only folder goes rootless", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    client.assertNoErrors(alpha);

    await client.changeWorkspaceFolders({ removed: ["alpha"] });
    await client.waitForRecompile(alpha);
    client.assertHasErrors(alpha, "the rootless project guesses a command");
});

test("definition crosses folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    await client.initialize(workspace, { folders: ["app", "lib"] });

    // The application's index only declares lib_fn; the library's defines
    // it, once its background index lands.
    const [main] = await client.openAndWait("app/main.cpp");
    expect(await client.waitForDefinition(main, 1, 21, workspace.uri("lib/src/lib.cpp"))).toBe(
        true,
    );
});

test("references cross folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    await client.initialize(workspace, { folders: ["app", "lib"] });

    const [lib] = await client.openAndWait("lib/src/lib.cpp");
    expect(await client.waitForIndex(lib, "main")).toBe(true);
    expect(await client.waitForReference(lib, 1, 5, workspace.uri("app/main.cpp"))).toBe(true);
});

test("unrelated folders keep references apart", async ({ session }) => {
    const { client, workspace } = session.tmp();
    const helper = (user: string) =>
        `int helper() { return 0; }\nint ${user}() { return helper(); }\n`;
    workspace.write("alpha/main.cpp", helper("use_alpha"));
    workspace.write("beta/main.cpp", helper("use_beta"));
    workspace.writeCDB(["alpha/main.cpp"], { at: "alpha/compile_commands.json" });
    workspace.writeCDB(["beta/main.cpp"], { at: "beta/compile_commands.json" });
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "use_alpha")).toBe(true);
    expect(await client.waitForIndex(alpha, "use_beta")).toBe(true);
    // Same name, same symbol id, but beta's index holds no file declaring
    // alpha's helper.
    expect(await client.referenceUris(alpha, 0, 4)).toEqual([workspace.uri("alpha/main.cpp")]);
});

test("batch index asks the folder's server", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "beta_fn")).toBe(true);
    const batch = spawnSync(
        cliceExecutable(),
        ["index", "--workspace", workspace.path("beta"), "--workers", "1"],
        { encoding: "utf8", timeout: INDEX_TIMEOUT },
    );
    expect(batch.status, `stderr: ${batch.stderr}`).toBe(0);
    expect(batch.stdout).toContain("through the running clice server");
});

test("indexing progress ends across folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    workspace.mkdir("empty");
    await client.initialize(workspace, { folders: ["alpha", "empty", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "alpha_fn")).toBe(true);
    expect(await client.waitForIndex(alpha, "beta_fn")).toBe(true);

    // A folder with nothing to index never runs a round; the others'
    // rounds still end the one the client sees. A round that ends before
    // the client acknowledged its token is never announced at all.
    const events = () =>
        client.progressEvents
            .filter((event) => event.token === "clice/backgroundIndex")
            .map((event) => event.value as { kind: string; message?: string });
    await waitUntil(() => [undefined, "end"].includes(events().at(-1)?.kind), {
        timeout: INDEX_TIMEOUT,
        interval: SETTLE_TIME,
        description: "the indexing progress to end",
    });
    let completed = 0;
    for (const event of events()) {
        if (event.kind === "begin") {
            completed = 0;
        } else if (event.kind === "report") {
            const count = Number(/^(\d+)\//.exec(event.message ?? "")?.[1]);
            expect(count, "a round's count never goes back").toBeGreaterThanOrEqual(completed);
            completed = count;
        }
    }
});

test("restart serves every folder", async ({ session }) => {
    const workspace = session.tmpdir();
    twoProjects(workspace);

    const first = await session
        .spawn(workspace)
        .initialize(workspace, { folders: ["alpha", "beta"] });
    await waitForDefinitionOf(first, "alpha_fn");
    await waitForDefinitionOf(first, "beta_fn");
    await first.shutdown();

    // Both folders load their persisted index at startup: the first query
    // answers before any worker could have reindexed either.
    const second = await session
        .spawn(workspace)
        .initialize(workspace, { folders: ["alpha", "beta"] });
    const symbols = (await second.workspaceSymbols("_fn")) ?? [];
    expect(symbols.map((symbol) => symbol.name).sort()).toEqual(["alpha_fn", "beta_fn"]);
    second.assertNoAnomaly();
});
