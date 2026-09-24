/// Several workspace folders served by one server: each folder is a project
/// with its own compilation database and cache, files are routed to the
/// project that compiles them, and folders come and go at runtime.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import { SETTLE_TIME, asLocations, waitUntil, type CliceClient } from "@clice/tools/client";
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

test("nested folder joins its project", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("outer/inner/main.cpp", gated("IN_OUTER", "inner_fn"));
    workspace.writeCDB(["outer/inner/main.cpp"], {
        extraArgs: ["-DIN_OUTER"],
        at: "outer/compile_commands.json",
    });
    // Listed first, the nested folder still is no project of its own.
    await client.initialize(workspace, { folders: ["outer/inner", "outer"] });

    const [inner] = await client.openAndWait("outer/inner/main.cpp");
    client.assertNoErrors(inner, "the enclosing project compiles it");
    // One project, holding the client's cache directory: neither folder
    // fell back to a default one.
    expect(fs.existsSync(workspace.path("outer/.clice"))).toBe(false);
    expect(fs.existsSync(workspace.path("outer/inner/.clice"))).toBe(false);

    const diagnosed = (errors: boolean, description: string) =>
        waitUntil(() => client.errors(inner).length > 0 === errors, {
            timeout: INDEX_TIMEOUT,
            interval: SETTLE_TIME,
            description,
        });
    // Alone, the nested folder is a project that knows no command for it.
    await client.changeWorkspaceFolders({ removed: ["outer"] });
    await diagnosed(true, "errors once the nested folder serves alone");
    await client.changeWorkspaceFolders({ added: ["outer"] });
    await diagnosed(false, "the enclosing project to take the folder back");
});

test("subproject serves what the folder does not build", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("mono/sub/clice.toml", "");
    workspace.write("mono/sub/tool.cpp", gated("IN_SUB", "tool_fn"));
    workspace.write("mono/sub/vendored.cpp", gated("IN_MONO", "vendored_fn"));
    workspace.writeCDB(["mono/sub/vendored.cpp"], {
        extraArgs: ["-DIN_MONO"],
        at: "mono/compile_commands.json",
    });
    workspace.writeCDB(["mono/sub/tool.cpp"], {
        extraArgs: ["-DIN_SUB"],
        at: "mono/sub/build/compile_commands.json",
    });
    await client.initialize(workspace, { folders: ["mono"] });

    const [vendored] = await client.openAndWait("mono/sub/vendored.cpp");
    client.assertNoErrors(vendored, "the folder's build compiles the file it lists");
    const [tool] = await client.openAndWait("mono/sub/tool.cpp");
    client.assertNoErrors(tool, "the subproject compiles the rest with its own database");
    client.assertNoErrors(vendored, "the listed file stays with the folder");
});

test("shared cache directory serves one project", ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("a/main.cpp", "int in_a() { return 0; }\n");
    workspace.write("b/main.cpp", "int in_b() { return 0; }\n");
    workspace.writeCDB(["a/main.cpp"], { at: "a/compile_commands.json" });
    workspace.writeCDB(["b/main.cpp"], { at: "b/compile_commands.json" });
    const shared = `[project]\ncache_dir = '${workspace.path("shared")}'\n`;
    workspace.write("a/clice.toml", shared);
    workspace.write("b/clice.toml", shared);
    const index = (folder: string) =>
        spawnSync(
            cliceExecutable(),
            ["index", "--workspace", workspace.path(folder), "--workers", "1"],
            { encoding: "utf8", timeout: INDEX_TIMEOUT },
        );

    expect(index("a").status).toBe(0);
    // Run after it, the other project indexes into a cache of its own
    // instead of the first one's.
    const second = index("b");
    expect(second.status, `stderr: ${second.stderr}`).toBe(0);
    expect(second.stdout).toContain("Indexed 1 translation unit");
    expect(fs.existsSync(workspace.path("b/.clice"))).toBe(true);
});

test("configuration menu per project", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    workspace.write(
        "beta/clice.toml",
        [
            'default_configuration = "fast"',
            "[[rules]]",
            'configuration = "fast"',
            'compile_commands = ["compile_commands.json"]',
            "[[rules]]",
            'configuration = "slow"',
            'compile_commands = ["compile_commands.json"]',
            "",
        ].join("\n"),
    );
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [alpha] = await client.openAndWait("alpha/main.cpp");
    const [beta] = await client.openAndWait("beta/main.cpp");
    expect((await client.listConfigurations(alpha)).configurations).toEqual([]);
    expect(await client.listConfigurations(beta)).toMatchObject({
        configurations: ["fast", "slow"],
        active: "fast",
    });
    expect(await client.switchConfiguration("slow", beta)).toEqual({ success: true });
    expect((await client.listConfigurations(beta)).selected).toBe("slow");
    expect((await client.listConfigurations()).configurations, "the first folder's").toEqual([]);
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

    // A moved document recompiles on its own: the client sends nothing
    // that would replace the diagnostics the old project published.
    const diagnosed = (errors: boolean, description: string) =>
        waitUntil(() => client.errors(beta).length > 0 === errors, {
            timeout: INDEX_TIMEOUT,
            interval: SETTLE_TIME,
            description,
        });
    await client.changeWorkspaceFolders({ removed: ["beta"] });
    await diagnosed(true, "errors from the remaining project, which has no command for it");

    await client.changeWorkspaceFolders({ added: ["beta"] });
    await diagnosed(false, "the re-added folder to serve it again");
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

test("database change moves a document", async ({ session }) => {
    const { client, workspace } = session.tmp();
    twoProjects(workspace);
    workspace.write("alpha/shared.cpp", gated("IN_BETA", "shared_fn"));
    await client.initialize(workspace, { folders: ["alpha", "beta"] });

    const [shared] = await client.openAndWait("alpha/shared.cpp");
    client.assertHasErrors(shared, "no database lists it yet");

    // Beta's database starts listing the file: it moves there.
    workspace.writeCDB(["beta/main.cpp", "alpha/shared.cpp"], {
        extraArgs: ["-DIN_BETA"],
        at: "beta/compile_commands.json",
    });
    await client.poll("cdb");
    await waitUntil(() => client.errors(shared).length === 0, {
        timeout: INDEX_TIMEOUT,
        interval: SETTLE_TIME,
        description: "beta to compile the file its database lists now",
    });
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

test("hierarchies cross folders", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    workspace.write(
        "lib/include/shape.h",
        "#pragma once\nstruct Shape {\n    virtual int area() const = 0;\n};\n",
    );
    workspace.write(
        "lib/src/lib.cpp",
        '#include "lib.h"\n#include "shape.h"\nint lib_fn() { return shared_fn(); }\n',
    );
    workspace.write(
        "app/main.cpp",
        '#include "lib.h"\n#include "shape.h"\n' +
            "struct Square : Shape {\n    int area() const override { return 4; }\n};\n" +
            "int main() { return lib_fn(); }\n",
    );
    await client.initialize(workspace, { folders: ["app", "lib"] });
    await waitForDefinitionOf(client, "main");
    await waitForDefinitionOf(client, "lib_fn", workspace.uri("lib/src/lib.cpp"));

    // The library's function is called from the application only.
    const [lib] = await client.openAndWait("lib/src/lib.cpp");
    const [fn] = (await client.prepareCallHierarchy(lib, 2, 5)) ?? [];
    expect(fn?.name).toBe("lib_fn");
    const callers = (await client.callHierarchyIncoming(fn!)) ?? [];
    expect(callers.map((call) => call.from.name)).toEqual(["main"]);

    // The library's interface is implemented in the application only.
    const [shape] = await client.openAndWait("lib/include/shape.h");
    const [base] = (await client.prepareTypeHierarchy(shape, 1, 8)) ?? [];
    expect(base?.name).toBe("Shape");
    const subtypes = (await client.typeHierarchySubtypes(base!)) ?? [];
    expect(subtypes.map((type) => type.name)).toEqual(["Square"]);
    const implementations = asLocations(await client.implementationAt(shape, 1, 8));
    expect(implementations.map((location) => location.uri)).toEqual([
        workspace.uri("app/main.cpp"),
    ]);
});

test("an open file answers through its project", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    await client.initialize(workspace, { folders: ["app", "lib"] });
    await waitForDefinitionOf(client, "main");
    await waitForDefinitionOf(client, "lib_fn", workspace.uri("lib/src/lib.cpp"));

    // Both projects index the header; the library serves it once open, and
    // its unsaved buffer moved the declaration a line down.
    const [header, text] = await client.openAndWait("lib/include/lib.h");
    const edited = client.armDiagnostics(header);
    client.change(header, 2, `// moved\n${text}`);
    await client.hoverAt(header, 2, 4);
    await edited;

    const [main] = await client.openAndWait("app/main.cpp");
    const references = (await client.referencesAt(main, 1, 21)) ?? [];
    const inHeader = references.filter((location) => location.uri === header);
    expect(inHeader.map((location) => location.range.start.line)).toEqual([2]);
});

test("dependency folder borrows the application", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("dep/include/dep.h", "#pragma once\n" + gated("IN_APP", "dep_fn"));
    workspace.write("app/main.cpp", '#include "dep.h"\nint main() { return dep_fn(); }\n');
    workspace.writeCDB(["app/main.cpp"], {
        extraArgs: [`-I${workspace.path("dep/include")}`, "-DIN_APP"],
        at: "app/compile_commands.json",
    });
    // The dependency folder has no database: its header compiles in the
    // context of the application including it.
    await client.initialize(workspace, { folders: ["app", "dep"] });

    const [header] = await client.openAndWait("dep/include/dep.h");
    client.assertNoErrors(header, "the application's command reaches the header");
});

test("context from another folder", async ({ session }) => {
    const { client, workspace } = session.tmp();
    libraryAndApp(workspace);
    workspace.write("lib/include/lib.h", "#pragma once\n" + gated("IN_APP", "lib_fn"));
    workspace.writeCDB(["app/main.cpp"], {
        extraArgs: [`-I${workspace.path("lib/include")}`, "-DIN_APP"],
        at: "app/compile_commands.json",
    });
    await client.initialize(workspace, { folders: ["app", "lib"] });

    // The library's own source is the header's host at first; the
    // application's is on offer too.
    const [header] = await client.openAndWait("lib/include/lib.h");
    client.assertHasErrors(header, "the library does not define IN_APP");
    const listed = await client.queryContext(header);
    const hosts = listed.contexts.map((context) => context.uri);
    expect(hosts).toContain(workspace.uri("lib/src/lib.cpp"));
    expect(hosts).toContain(workspace.uri("app/main.cpp"));

    const compiled = client.armDiagnostics(header);
    const switched = await client.switchContext(header, workspace.uri("app/main.cpp"), {
        epoch: listed.epoch,
    });
    expect(switched.success).toBe(true);
    await client.hoverAt(header, 1, 0);
    await compiled;
    client.assertNoErrors(header, "the application's host defines IN_APP");
    expect((await client.currentContext(header)).context?.uri).toBe(
        workspace.uri("app/main.cpp"),
    );
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
