/// Several workspace folders served by one server: each folder is a project
/// with its own compilation database and cache, files are routed to the
/// project that compiles them, and folders come and go at runtime.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { runProcess, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { at, cliceExecutable, expect, serve } from "../../fixtures.ts";

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

/// One file both folders' databases list, each under its own flag.
function sharedFile(ws: Workspace): void {
    ws.write("alpha/shared.cpp", "int shared() { return 0; }\n");
    ws.writeCDB(["alpha/shared.cpp"], {
        extraArgs: ["-DFIRST"],
        at: "alpha/compile_commands.json",
    });
    ws.writeCDB(["alpha/shared.cpp"], {
        extraArgs: ["-DSECOND"],
        at: "beta/compile_commands.json",
    });
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

/// Start a server announcing `folders` (InitializeOptions.folders) and
/// point the case's actions at it: a Serve server announces the workspace
/// root alone.
async function startOn(
    s: Serve,
    folders: string[] | null,
    beforeInitialized?: (client: CliceClient) => Promise<void>,
): Promise<void> {
    const client = s.session.spawn(s.workspace);
    (s as unknown as { server: CliceClient | null }).server = client;
    await client.initialize(s.workspace, {
        folders,
        beforeInitialized:
            beforeInitialized === undefined ? undefined : () => beforeInitialized(client),
    });
}

/// Serve `folders` over the workspace `layout` writes, in place of the
/// empty workspace the case started on: its server, cache and database go.
async function onFolders(
    s: Serve,
    folders: string[] | null,
    layout: (ws: Workspace) => void,
    beforeInitialized?: (client: CliceClient) => Promise<void>,
): Promise<void> {
    await s.stop();
    s.workspace.rm(".clice");
    s.workspace.rm("compile_commands.json");
    layout(s.workspace);
    await startOn(s, folders, beforeInitialized);
}

/// Folders come and go (didChangeWorkspaceFolders); the server settles.
async function changeFolders(
    s: Serve,
    change: { added?: string[]; removed?: string[] },
): Promise<void> {
    s.steps.note(`folders ${JSON.stringify(change)}`);
    await s.client.changeWorkspaceFolders(change);
    await s.sync();
}

/// The errors the server last pushed for `file`, unasked.
function pushedErrors(s: Serve, file: string): proto.Diagnostic[] {
    return s.client.errors(s.uri(file));
}

/// The index lists a definition of `name` at `where` ("file: its line").
async function listed(s: Serve, name: string, where: string): Promise<void> {
    expect(s.show(await s.workspaceSymbols(name)).split("\n"), `${name} indexed`).toContain(
        `${name} ${where}`,
    );
}

/// The lines of a rendered reply that point into `file`.
function into(s: Serve, reply: unknown, file: string): string[] {
    return s
        .show(reply)
        .split("\n")
        .filter((line) => line.startsWith(`${file}: `));
}

const ALPHA_FN = "alpha/main.cpp: int alpha_fn() { return 0; }";
const BETA_FN = "beta/main.cpp: int beta_fn() { return 0; }";
const MAIN = "app/main.cpp: int main() { return lib_fn(); }";
const LIB_FN = "lib/src/lib.cpp: int lib_fn() { return shared_fn(); }";
const LIB_FN_CALL = at("app/main.cpp", "l|ib_fn()");

const test = serve.files({});

test("folders compile separately", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    expect(await s.errors("alpha/main.cpp"), "alpha compiles with its own database").toEqual([]);
    expect(await s.errors("beta/main.cpp"), "beta compiles with its own database").toEqual([]);

    const stats = await s.client.stats();
    expect(stats.sessions, "the gauges add every folder up").toBe(2);
    expect((await s.client.poll("cdb")).events, "a tick finds nothing changed").toBe(0);
});

test("root uri alone serves its folder", async ({ s }) => {
    await onFolders(s, null, twoProjects);
    expect(
        await s.errors("alpha/main.cpp"),
        "the root's project finds the database below it",
    ).toEqual([]);
});

test("workspace symbol spans folders", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "alpha_fn", ALPHA_FN);
    await listed(s, "beta_fn", BETA_FN);
});

test("shared header symbol listed once", async ({ s }) => {
    await onFolders(s, ["app", "lib"], libraryAndApp);
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);
    const symbols = (await s.workspaceSymbols("shared_fn")) ?? [];
    expect(symbols.map((symbol) => symbol.name)).toEqual(["shared_fn"]);
});

test("each folder keeps its own cache", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "beta_fn", BETA_FN);
    await s.stop();
    // The client's cache directory went to the first folder; the second
    // kept its default.
    expect(s.workspace.exists(".clice")).toBe(true);
    expect(s.workspace.exists("beta/.clice")).toBe(true);
});

test("configured cache directory serves once", async ({ s }) => {
    await onFolders(s, ["alpha", "beta", "gamma"], (ws) => {
        twoProjects(ws);
        ws.write("gamma/main.cpp", "int gamma_fn() { return 0; }\n");
        ws.writeCDB(["gamma/main.cpp"], { at: "gamma/compile_commands.json" });
        const shared = `[project]\ncache_dir = "${ws.path("shared").replaceAll("\\", "/")}"\n`;
        for (const folder of ["alpha", "beta", "gamma"]) {
            ws.write(`${folder}/clice.toml`, shared);
        }
    });
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "gamma_fn", "gamma/main.cpp: int gamma_fn() { return 0; }");
    await s.stop();
    // alpha takes the client's directory and beta its clice.toml's; gamma
    // finds both taken and falls back to its default.
    expect(s.workspace.exists(".clice")).toBe(true);
    expect(s.workspace.exists("shared")).toBe(true);
    expect(s.workspace.exists("gamma/.clice")).toBe(true);
});

test("nested folder joins its project", async ({ s }) => {
    const inner = "outer/inner/main.cpp";
    // Listed first, the nested folder still is no project of its own.
    await onFolders(s, ["outer/inner", "outer"], (ws) => {
        ws.write(inner, gated("IN_OUTER", "inner_fn"));
        ws.writeCDB([inner], { extraArgs: ["-DIN_OUTER"], at: "outer/compile_commands.json" });
    });
    expect(await s.errors(inner), "the enclosing project compiles it").toEqual([]);
    // One project, holding the client's cache directory: neither folder
    // fell back to a default one.
    expect(s.workspace.exists("outer/.clice")).toBe(false);
    expect(s.workspace.exists("outer/inner/.clice")).toBe(false);

    // Alone, the nested folder is a project that knows no command for it.
    await changeFolders(s, { removed: ["outer"] });
    expect(
        pushedErrors(s, inner).length,
        "errors once the nested folder serves alone",
    ).toBeGreaterThan(0);
    await changeFolders(s, { added: ["outer"] });
    expect(pushedErrors(s, inner), "the enclosing project to take the folder back").toEqual([]);
});

test("subproject serves what the folder does not build", async ({ s }) => {
    const tool = "mono/sub/tool.cpp";
    const vendored = "mono/sub/vendored.cpp";
    await onFolders(s, ["mono"], (ws) => {
        ws.write("mono/sub/clice.toml", "");
        ws.write(tool, gated("IN_SUB", "tool_fn"));
        ws.write(vendored, gated("IN_MONO", "vendored_fn"));
        ws.writeCDB([vendored], { extraArgs: ["-DIN_MONO"], at: "mono/compile_commands.json" });
        ws.writeCDB([tool], {
            extraArgs: ["-DIN_SUB"],
            at: "mono/sub/build/compile_commands.json",
        });
    });
    expect(await s.errors(vendored), "the folder's build compiles the file it lists").toEqual([]);
    expect(await s.errors(tool), "the subproject compiles the rest with its own database").toEqual(
        [],
    );
    expect(await s.errors(vendored), "the listed file stays with the folder").toEqual([]);
});

test("configuration menu per project", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], (ws) => {
        twoProjects(ws);
        ws.write(
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
    });
    await s.compiled("alpha/main.cpp");
    await s.compiled("beta/main.cpp");
    const alpha = s.uri("alpha/main.cpp");
    const beta = s.uri("beta/main.cpp");
    expect((await s.client.listConfigurations(alpha)).configurations).toEqual([]);
    expect(await s.client.listConfigurations(beta)).toMatchObject({
        configurations: ["fast", "slow"],
        active: "fast",
    });
    expect(await s.client.switchConfiguration("slow", beta)).toEqual({ success: true });
    expect((await s.client.listConfigurations(beta)).selected).toBe("slow");
    expect((await s.client.listConfigurations()).configurations, "the first folder's").toEqual([]);
});

test("unclaimed file opens its project", async ({ s }) => {
    await onFolders(s, ["alpha"], twoProjects);
    // Outside every folder, with a database above it: that folder is
    // served as if it were open.
    expect(await s.errors("beta/main.cpp"), "the project found above the file compiles it").toEqual(
        [],
    );
});

test("rootless server finds projects", async ({ s }) => {
    await onFolders(s, [], twoProjects);
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
});

test("folder change before initialized", async ({ s }) => {
    await onFolders(s, ["alpha"], twoProjects, (client) =>
        client.changeWorkspaceFolders({ added: ["beta"], removed: ["alpha"] }),
    );
    await s.indexed();
    await listed(s, "beta_fn", BETA_FN);
    expect((await s.workspaceSymbols("alpha_fn")) ?? []).toEqual([]);
});

test("added folder adopts its files", async ({ s }) => {
    await onFolders(s, ["alpha"], (ws) => {
        twoProjects(ws);
        ws.rm("beta/compile_commands.json");
    });
    // Nothing above it knows the file: the first project serves it, without
    // beta's flags.
    expect(
        (await s.errors("beta/main.cpp")).length,
        "no project knows beta's flags yet",
    ).toBeGreaterThan(0);

    s.workspace.writeCDB(["beta/main.cpp"], {
        extraArgs: ["-DIN_BETA"],
        at: "beta/compile_commands.json",
    });
    await changeFolders(s, { added: ["beta"] });
    expect(
        await s.errors("beta/main.cpp"),
        "the new folder's project compiles the open file",
    ).toEqual([]);
});

test("removed folder releases its files", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    expect(await s.errors("beta/main.cpp")).toEqual([]);

    // A moved document recompiles on its own: the client sends nothing
    // that would replace the diagnostics the old project published.
    await changeFolders(s, { removed: ["beta"] });
    expect(
        pushedErrors(s, "beta/main.cpp").length,
        "errors from the remaining project, which has no command for it",
    ).toBeGreaterThan(0);

    await changeFolders(s, { added: ["beta"] });
    expect(pushedErrors(s, "beta/main.cpp"), "the re-added folder to serve it again").toEqual([]);
});

test("folder re-added at once keeps its cache", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    await s.indexed();
    await listed(s, "beta_fn", BETA_FN);

    // The control endpoint record appears only while a project holds the
    // cache directory's writer lock.
    const record = "beta/.clice/server.json";
    expect(s.workspace.exists(record)).toBe(true);
    const before = s.disk.read(record);

    s.steps.note("folders: beta removed and added back");
    await s.client.changeWorkspaceFolders({ removed: ["beta"] });
    await s.client.changeWorkspaceFolders({ added: ["beta"] });
    await s.sync();
    expect(s.workspace.exists(record), "the re-added folder to take its cache directory back").toBe(
        true,
    );
    expect(s.disk.read(record)).not.toBe(before);
});

test("database change moves a document", async ({ s }) => {
    const shared = "alpha/shared.cpp";
    await onFolders(s, ["alpha", "beta"], (ws) => {
        twoProjects(ws);
        ws.write(shared, gated("IN_BETA", "shared_fn"));
    });
    expect((await s.errors(shared)).length, "no database lists it yet").toBeGreaterThan(0);

    // Beta's database starts listing the file: it moves there.
    s.workspace.writeCDB(["beta/main.cpp", shared], {
        extraArgs: ["-DIN_BETA"],
        at: "beta/compile_commands.json",
    });
    await s.client.poll("cdb");
    await s.sync();
    expect(pushedErrors(s, shared), "beta to compile the file its database lists now").toEqual([]);
});

test("removed first folder hands over", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    await s.compiled("alpha/main.cpp");
    await s.compiled("beta/main.cpp");

    await changeFolders(s, { removed: ["alpha"] });
    expect(
        (await s.errors("alpha/main.cpp")).length,
        "beta serves the file, without alpha's flags",
    ).toBeGreaterThan(0);
    expect(
        await s.hover(at("beta/main.cpp", "int |beta_fn")),
        "beta keeps serving its own",
    ).not.toBeNull();
});

test("removed only folder goes rootless", async ({ s }) => {
    await onFolders(s, ["alpha"], twoProjects);
    expect(await s.errors("alpha/main.cpp")).toEqual([]);

    await changeFolders(s, { removed: ["alpha"] });
    expect(
        (await s.errors("alpha/main.cpp")).length,
        "the rootless project guesses a command",
    ).toBeGreaterThan(0);
});

test("definition crosses folders", async ({ s }) => {
    await onFolders(s, ["app", "lib"], libraryAndApp);
    // The application's index only declares lib_fn; the library's defines
    // it, once its background index lands.
    await s.compiled("app/main.cpp");
    await s.indexed();
    expect(s.show(await s.definition(LIB_FN_CALL)).split("\n")).toContain(LIB_FN);
});

test("references cross folders", async ({ s }) => {
    await onFolders(s, ["app", "lib"], libraryAndApp);
    await s.compiled("lib/src/lib.cpp");
    await s.indexed();
    await listed(s, "main", MAIN);
    expect(s.show(await s.references(at("lib/src/lib.cpp", "int l|ib_fn"))).split("\n")).toContain(
        MAIN,
    );
});

test("shared macro references cross folders", async ({ s }) => {
    // The macro's id takes its header relative to the library's root in
    // the library and absolute in the application; the two meet at its
    // definition.
    await onFolders(s, ["app", "lib"], (ws) => {
        libraryAndApp(ws);
        ws.write("lib/include/lib.h", "#pragma once\n#define LIB_LIMIT 4\nint lib_fn();\n");
        ws.write("lib/src/lib.cpp", '#include "lib.h"\nint lib_fn() { return LIB_LIMIT; }\n');
        ws.write("app/main.cpp", '#include "lib.h"\nint main() { return LIB_LIMIT + lib_fn(); }\n');
    });
    await s.compiled("lib/src/lib.cpp");
    await s.indexed();
    await listed(s, "main", "app/main.cpp: int main() { return LIB_LIMIT + lib_fn(); }");
    expect(s.show(await s.references(at("lib/src/lib.cpp", "L|IB_LIMIT"))).split("\n")).toContain(
        "app/main.cpp: int main() { return LIB_LIMIT + lib_fn(); }",
    );
});

test("hierarchies cross folders", async ({ s }) => {
    await onFolders(s, ["app", "lib"], (ws) => {
        libraryAndApp(ws);
        ws.write(
            "lib/include/shape.h",
            "#pragma once\nstruct Shape {\n    virtual int area() const = 0;\n};\n",
        );
        ws.write(
            "lib/src/lib.cpp",
            '#include "lib.h"\n#include "shape.h"\nint lib_fn() { return shared_fn(); }\n',
        );
        ws.write(
            "app/main.cpp",
            '#include "lib.h"\n#include "shape.h"\n' +
                "struct Square : Shape {\n    int area() const override { return 4; }\n};\n" +
                "int main() { return lib_fn(); }\n",
        );
    });
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);

    // The library's function is called from the application only.
    await s.compiled("lib/src/lib.cpp");
    const [fn] = ((await s.request(
        "textDocument/prepareCallHierarchy",
        at("lib/src/lib.cpp", "int l|ib_fn"),
    )) ?? []) as proto.CallHierarchyItem[];
    expect(fn?.name).toBe("lib_fn");
    const callers = (await s.client.callHierarchyIncoming(fn!)) ?? [];
    expect(callers.map((call) => call.from.name)).toEqual(["main"]);

    // The library's interface is implemented in the application only.
    const shape = at("lib/include/shape.h", "struct S|hape");
    await s.compiled("lib/include/shape.h");
    const [base] = ((await s.request("textDocument/prepareTypeHierarchy", shape)) ??
        []) as proto.TypeHierarchyItem[];
    expect(base?.name).toBe("Shape");
    const subtypes = (await s.client.typeHierarchySubtypes(base!)) ?? [];
    expect(subtypes.map((type) => type.name)).toEqual(["Square"]);
    expect(s.show(await s.request("textDocument/implementation", shape))).toBe(
        "app/main.cpp: struct Square : Shape {",
    );

    // The other way round the library knows nothing of the application's
    // type, yet its base stays reachable though the library serves the
    // base's open file.
    await s.compiled("app/main.cpp");
    const [square] = ((await s.request(
        "textDocument/prepareTypeHierarchy",
        at("app/main.cpp", "struct S|quare"),
    )) ?? []) as proto.TypeHierarchyItem[];
    expect(square?.name).toBe("Square");
    const supertypes = (await s.client.typeHierarchySupertypes(square!)) ?? [];
    expect(supertypes.map((type) => type.name)).toEqual(["Shape"]);
});

test("definition follows an open buffer", async ({ s }) => {
    // The application builds the library's source too; the library, listed
    // first, serves it once open.
    await onFolders(s, ["lib", "app"], (ws) => {
        libraryAndApp(ws);
        ws.writeCDB(["app/main.cpp", "lib/src/lib.cpp"], {
            extraArgs: [`-I${ws.path("lib/include")}`],
            at: "app/compile_commands.json",
        });
    });
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);

    await s.compiled("lib/src/lib.cpp");
    s.edit("lib/src/lib.cpp", { before: '#include "lib.h"', insert: "// moved\n" });
    await s.compiled("lib/src/lib.cpp");

    // Rendered from the buffer: the definition's line is the one it moved to.
    await s.compiled("app/main.cpp");
    expect(s.show(await s.definition(LIB_FN_CALL))).toBe(LIB_FN);
});

test("an open file answers through its project", async ({ s }) => {
    await onFolders(s, ["app", "lib"], libraryAndApp);
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);

    // Both projects index the header; the library serves it once open, and
    // its unsaved buffer moved the declaration a line down.
    const header = "lib/include/lib.h";
    await s.compiled(header);
    s.edit(header, { before: "#pragma once", insert: "// moved\n" });
    await s.compiled(header);

    await s.compiled("app/main.cpp");
    expect(into(s, await s.references(LIB_FN_CALL), header)).toEqual([`${header}: int lib_fn();`]);
});

test("dependency folder borrows the application", async ({ s }) => {
    // The dependency folder has no database: its header compiles in the
    // context of the application including it.
    await onFolders(s, ["app", "dep"], (ws) => {
        ws.write("dep/include/dep.h", "#pragma once\n" + gated("IN_APP", "dep_fn"));
        ws.write("app/main.cpp", '#include "dep.h"\nint main() { return dep_fn(); }\n');
        ws.writeCDB(["app/main.cpp"], {
            extraArgs: [`-I${ws.path("dep/include")}`, "-DIN_APP"],
            at: "app/compile_commands.json",
        });
    });
    expect(
        await s.errors("dep/include/dep.h"),
        "the application's command reaches the header",
    ).toEqual([]);
});

test("context from another folder", async ({ s }) => {
    await onFolders(s, ["app", "lib"], (ws) => {
        libraryAndApp(ws);
        ws.write("lib/include/lib.h", "#pragma once\n" + gated("IN_APP", "lib_fn"));
        ws.writeCDB(["app/main.cpp"], {
            extraArgs: [`-I${ws.path("lib/include")}`, "-DIN_APP"],
            at: "app/compile_commands.json",
        });
    });
    const header = "lib/include/lib.h";
    const uri = s.uri(header);

    // The library's own source is the header's host at first; the
    // application's is on offer too.
    expect((await s.errors(header)).length, "the library does not define IN_APP").toBeGreaterThan(
        0,
    );
    const listed = await s.client.queryContext(uri);
    const hosts = listed.contexts.map((context) => context.uri);
    expect(hosts).toContain(s.uri("lib/src/lib.cpp"));
    expect(hosts).toContain(s.uri("app/main.cpp"));

    const switched = await s.client.switchContext(uri, s.uri("app/main.cpp"), {
        epoch: listed.epoch,
    });
    expect(switched.success).toBe(true);
    expect(await s.errors(header), "the application's host defines IN_APP").toEqual([]);
    expect((await s.client.currentContext(uri)).context?.uri).toBe(s.uri("app/main.cpp"));

    // The reset hands the header back to its own folder's host; the
    // application's project retracts its diagnostics first.
    expect((await s.client.resetContext(uri)).success).toBe(true);
    await s.sync();
    expect(pushedErrors(s, header).length, "the library does not define IN_APP").toBeGreaterThan(0);
    const current = await s.client.currentContext(uri);
    expect(current.automatic).toBe(true);
    expect(current.context?.uri).toBe(s.uri("lib/src/lib.cpp"));
});

test("own configuration of another folder", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], sharedFile);

    // Both databases list the file: its owner's entry comes first, the
    // other project's can be switched to, moving the file there.
    await s.compiled("alpha/shared.cpp");
    const shared = s.uri("alpha/shared.cpp");
    const listed = await s.client.queryContext(shared);
    const own = listed.contexts.filter((context) => context.uri === shared);
    expect(own).toHaveLength(2);
    const other = own[1]!.commandHash!;
    const switched = await s.client.switchContext(shared, shared, {
        commandHash: other,
        epoch: listed.epoch,
    });
    expect(switched.success).toBe(true);
    expect((await s.client.currentContext(shared)).context?.commandHash).toBe(other);
});

test("a closed file keeps its choice", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], sharedFile);

    await s.compiled("alpha/shared.cpp");
    const shared = s.uri("alpha/shared.cpp");
    const listed = await s.client.queryContext(shared);
    const own = listed.contexts.filter((context) => context.uri === shared);
    expect(own).toHaveLength(2);
    const first = own[0]!.commandHash!;
    const other = own[1]!.commandHash!;
    const switched = await s.client.switchContext(shared, shared, {
        commandHash: other,
        epoch: listed.epoch,
    });
    expect(switched.success).toBe(true);

    s.close("alpha/shared.cpp");
    const refused = await s.client.switchContext(shared, shared, { commandHash: first });
    expect(refused.success).toBe(false);
    await s.compiled("alpha/shared.cpp");
    expect((await s.client.currentContext(shared)).context?.commandHash).toBe(other);
});

test("save reaches every folder", async ({ s }) => {
    await onFolders(s, ["app", "lib"], libraryAndApp);
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);

    // Saved from the library, the header moves its declaration a line down;
    // the application, which includes it too, reindexes as well.
    const header = "lib/include/lib.h";
    await s.compiled(header);
    s.edit(header, { before: "#pragma once", insert: "// moved\n" });
    s.save(header);
    s.close(header);

    await s.compiled("app/main.cpp");
    await s.sync();
    expect(into(s, await s.references(LIB_FN_CALL), header), "both folders reindexed").toEqual([
        `${header}: int lib_fn();`,
    ]);
});

test("unrelated folders keep references apart", async ({ s }) => {
    const helper = (user: string) =>
        `int helper() { return 0; }\nint ${user}() { return helper(); }\n`;
    await onFolders(s, ["alpha", "beta"], (ws) => {
        ws.write("alpha/main.cpp", helper("use_alpha"));
        ws.write("beta/main.cpp", helper("use_beta"));
        ws.writeCDB(["alpha/main.cpp"], { at: "alpha/compile_commands.json" });
        ws.writeCDB(["beta/main.cpp"], { at: "beta/compile_commands.json" });
    });
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "use_alpha", "alpha/main.cpp: int use_alpha() { return helper(); }");
    await listed(s, "use_beta", "beta/main.cpp: int use_beta() { return helper(); }");
    // Same name, same symbol id, but beta's index holds no file declaring
    // alpha's helper.
    const references = await s.request(
        "textDocument/references",
        at("alpha/main.cpp", "int |helper"),
        { context: { includeDeclaration: false } },
    );
    expect(s.show(references)).toBe("alpha/main.cpp: int use_alpha() { return helper(); }");
});

test("batch index asks the folder's server", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "beta_fn", BETA_FN);
    const batch = await runProcess(cliceExecutable(), [
        "index",
        "--workspace",
        s.workspace.path("beta"),
        "--workers",
        "1",
    ]);
    expect(batch.status, `stderr: ${batch.stderr}`).toBe(0);
    expect(batch.stdout).toContain("through the running clice server");
});

test("indexing progress ends across folders", async ({ s }) => {
    await onFolders(s, ["alpha", "empty", "beta"], (ws) => {
        twoProjects(ws);
        ws.mkdir("empty");
    });
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "alpha_fn", ALPHA_FN);
    await listed(s, "beta_fn", BETA_FN);

    // A folder with nothing to index never runs a round; the others'
    // rounds still end the one the client sees. A round that ends before
    // the client acknowledged its token is never announced at all.
    const events = s.client.progressEvents
        .filter((event) => event.token === "clice/backgroundIndex")
        .map((event) => event.value as { kind: string; message?: string });
    expect([undefined, "end"], "the indexing progress to end").toContain(events.at(-1)?.kind);
    let completed = 0;
    for (const event of events) {
        if (event.kind === "begin") {
            completed = 0;
        } else if (event.kind === "report") {
            const count = Number(/^(\d+)\//.exec(event.message ?? "")?.[1]);
            expect(count, "a round's count never goes back").toBeGreaterThanOrEqual(completed);
            completed = count;
        }
    }
});

test("restart serves every folder", async ({ s }) => {
    await onFolders(s, ["alpha", "beta"], twoProjects);
    await s.indexed();
    await listed(s, "alpha_fn", ALPHA_FN);
    await listed(s, "beta_fn", BETA_FN);
    await s.stop();

    // Both folders load their persisted index at startup: the first query
    // answers before any worker could have reindexed either.
    await startOn(s, ["alpha", "beta"]);
    const symbols = (await s.workspaceSymbols("_fn")) ?? [];
    expect(symbols.map((symbol) => symbol.name).sort()).toEqual(["alpha_fn", "beta_fn"]);
    s.client.assertNoAnomaly();
});
