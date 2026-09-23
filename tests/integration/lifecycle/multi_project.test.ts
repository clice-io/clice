/// Several workspace folders served by one server: each folder is a project
/// with its own compilation database and cache, files are routed to the
/// project that compiles them, and folders come and go at runtime.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

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

function changeFolders(
    client: CliceClient,
    ws: Workspace,
    change: { added?: string[]; removed?: string[] },
): Promise<void> {
    const folder = (name: string) => ({ uri: URI.file(ws.path(name)).toString(), name });
    return client.sendNotification(proto.DidChangeWorkspaceFoldersNotification.type, {
        event: {
            added: (change.added ?? []).map(folder),
            removed: (change.removed ?? []).map(folder),
        },
    });
}

test("folders compile separately", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertNoErrors(alpha, "alpha compiles with its own database");
    client.assertNoErrors(beta, "beta compiles with its own database");
});

test("workspace symbol spans folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "alpha_fn"), "alpha's symbol").toBe(true);
    expect(await client.waitForIndex(alpha, "beta_fn"), "beta's symbol").toBe(true);
});

test("each folder keeps its own cache", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    workspace.write("alpha/clice.toml", '[project]\ncache_dir = "${workspace}/.clice"\n');
    workspace.write("beta/clice.toml", '[project]\ncache_dir = "${workspace}/.clice"\n');
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    expect(await client.waitForIndex(alpha, "beta_fn")).toBe(true);
    await client.shutdown();
    // The client's cache directory went to the first folder; the second
    // kept the one its configuration names.
    expect(fs.existsSync(workspace.path(".clice"))).toBe(true);
    expect(fs.existsSync(workspace.path("beta/.clice"))).toBe(true);
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
    await changeFolders(client, workspace, { added: ["beta"] });
    await client.waitForRecompile(beta);
    client.assertNoErrors(beta, "the new folder's project compiles the open file");
});

test("removed folder releases its files", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [beta] = await client.openAndWait("beta/main.cpp");
    client.assertNoErrors(beta);

    await changeFolders(client, workspace, { removed: ["beta"] });
    await client.waitForRecompile(beta);
    client.assertHasErrors(beta, "the remaining project has no command for it");
});

/// A library and an application including its header, each its own folder.
function libraryAndApp(ws: Workspace): void {
    ws.write("lib/include/lib.h", "#pragma once\nint lib_fn();\n");
    ws.write("lib/src/lib.cpp", '#include "lib.h"\nint lib_fn() { return 1; }\n');
    ws.write("app/main.cpp", '#include "lib.h"\nint main() { return lib_fn(); }\n');
    const include = `-I${ws.path("lib/include")}`;
    ws.writeCDB(["lib/src/lib.cpp"], { extraArgs: [include], at: "lib/compile_commands.json" });
    ws.writeCDB(["app/main.cpp"], { extraArgs: [include], at: "app/compile_commands.json" });
}

test("definition crosses folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    await client.initialize(workspace, { folders: ["app", "lib"] });

    const [main] = await client.openAndWait("app/main.cpp");

    // The application's index only declares lib_fn; the library's defines
    // it, once its background index lands.
    const definitionUris = async () => {
        const locations = await client.definitionAt(main, 1, 21);
        return (Array.isArray(locations) ? locations : locations ? [locations] : []).map(
            (location) => ("uri" in location ? location.uri : location.targetUri),
        );
    };
    await waitUntil(
        async () => (await definitionUris()).includes(workspace.uri("lib/src/lib.cpp")),
        { timeout: 30_000, interval: 500, description: "the library's definition of lib_fn" },
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

test("restart serves every folder", async ({ session }) => {
    const workspace = session.tmpdir();
    twoProjects(workspace);
    workspace.write("alpha/clice.toml", '[project]\ncache_dir = "${workspace}/.clice"\n');
    workspace.write("beta/clice.toml", '[project]\ncache_dir = "${workspace}/.clice"\n');

    const first = await session
        .spawn(workspace)
        .initialize(workspace, { folders: ["alpha", "beta"] });
    const [alpha] = await first.openAndWait("alpha/main.cpp");
    expect(await first.waitForIndex(alpha, "alpha_fn")).toBe(true);
    expect(await first.waitForIndex(alpha, "beta_fn")).toBe(true);
    await first.shutdown();

    // Both folders persisted an index; the second to load finds the shared
    // file table taken and rebuilds its own.
    const second = await session
        .spawn(workspace)
        .initialize(workspace, { folders: ["alpha", "beta"] });
    const [again] = await second.openAndWait("alpha/main.cpp");
    expect(await second.waitForIndex(again, "alpha_fn")).toBe(true);
    expect(await second.waitForIndex(again, "beta_fn")).toBe(true);
    second.assertNoAnomaly();
});
