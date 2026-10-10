/// Several workspace folders served by one server: each folder is a project
/// with its own compilation database and cache, files are routed to the
/// project that compiles them, and folders come and go at runtime.

import * as proto from "vscode-languageserver-protocol";
import type { FileText, Serve, ServeOptions } from "@clice/tools/actions";
import { runProcess } from "@clice/tools/client";
import type { Manifest } from "@clice/tools/project";
import type { Workspace } from "@clice/tools/workspace";
import { at, cliceExecutable, expect, serve, type ServeTest } from "../../fixtures.ts";

/// A source that compiles only with `-D<flag>`, defining `name`.
function gated(flag: string, name: string): string {
    return `#ifndef ${flag}\n#error missing ${flag}\n#endif\nint ${name}() { return 0; }\n`;
}

/// A database of `units`, each with its own arguments.
function database(units: Record<string, string[]>): Manifest {
    return { cxx: ["-std=c++17"], units };
}

/// Files and the databases listing them, by workspace-relative path.
interface Layout {
    files: Record<string, FileText>;
    databases: Record<string, Manifest>;
}

/// Cases on `layout`, every server announcing `announced` as its folders.
function folders(
    announced: string[] | null,
    layout: Layout,
    options: ServeOptions = {},
): ServeTest {
    return serve.files(layout.files, {
        ...options,
        databases: layout.databases,
        launch: { ...options.launch, folders: announced },
    });
}

const ALPHA_DATABASE = database({ "alpha/main.cpp": ["-DIN_ALPHA"] });
const BETA_DATABASE = database({ "beta/main.cpp": ["-DIN_BETA"] });

/// Two folders, each with a database that passes its own flag.
const TWO_PROJECTS: Layout = {
    files: {
        "alpha/main.cpp": gated("IN_ALPHA", "alpha_fn"),
        "beta/main.cpp": gated("IN_BETA", "beta_fn"),
    },
    databases: {
        "alpha/compile_commands.json": ALPHA_DATABASE,
        "beta/compile_commands.json": BETA_DATABASE,
    },
};

/// One file both folders' databases list, each under its own flag.
const SHARED_FILE: Layout = {
    files: { "alpha/shared.cpp": "int shared() { return 0; }\n" },
    databases: {
        "alpha/compile_commands.json": database({ "alpha/shared.cpp": ["-DFIRST"] }),
        "beta/compile_commands.json": database({ "alpha/shared.cpp": ["-DSECOND"] }),
    },
};

const INCLUDE_LIB = "-I${workspace}/lib/include";

/// A library and an application including its header, each its own folder.
const LIBRARY_AND_APP: Layout = {
    files: {
        "lib/include/lib.h": "#pragma once\nint lib_fn();\ninline int shared_fn() { return 2; }\n",
        "lib/src/lib.cpp": '#include "lib.h"\nint lib_fn() { return shared_fn(); }\n',
        "app/main.cpp": '#include "lib.h"\nint main() { return lib_fn(); }\n',
    },
    databases: {
        "lib/compile_commands.json": database({ "lib/src/lib.cpp": [INCLUDE_LIB] }),
        "app/compile_commands.json": database({ "app/main.cpp": [INCLUDE_LIB] }),
    },
};

/// The errors the server last pushed for `file`, unasked; undefined when
/// it pushed none.
async function pushedErrors(s: Serve, file: string): Promise<proto.Diagnostic[] | undefined> {
    return (await s.pushed(file))?.filter(
        (diagnostic) => diagnostic.severity === proto.DiagnosticSeverity.Error,
    );
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

folders(["alpha", "beta"], TWO_PROJECTS)("folders compile separately", async ({ s }) => {
    expect(await s.errors("alpha/main.cpp"), "alpha compiles with its own database").toEqual([]);
    expect(await s.errors("beta/main.cpp"), "beta compiles with its own database").toEqual([]);

    expect((await s.stats()).sessions, "the gauges add every folder up").toBe(2);
    expect(await s.poll("cdb"), "a tick finds nothing changed").toBe(0);
});

folders(null, TWO_PROJECTS)("root uri alone serves its folder", async ({ s }) => {
    expect(
        await s.errors("alpha/main.cpp"),
        "the root's project finds the database below it",
    ).toEqual([]);
});

folders(["alpha", "beta"], TWO_PROJECTS)("workspace symbol spans folders", async ({ s }) => {
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "alpha_fn", ALPHA_FN);
    await listed(s, "beta_fn", BETA_FN);
});

folders(["app", "lib"], LIBRARY_AND_APP)("shared header symbol listed once", async ({ s }) => {
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);
    const symbols = (await s.workspaceSymbols("shared_fn")) ?? [];
    expect(symbols.map((symbol) => symbol.name)).toEqual(["shared_fn"]);
});

folders(["alpha", "beta"], TWO_PROJECTS)("each folder keeps its own cache", async ({ s }) => {
    await s.compiled("alpha/main.cpp");
    await s.indexed();
    await listed(s, "beta_fn", BETA_FN);
    await s.stop();
    // The client's cache directory went to the first folder; the second
    // kept its default.
    expect(s.workspace.exists(".clice")).toBe(true);
    expect(s.workspace.exists("beta/.clice")).toBe(true);
});

const SHARED_CACHE = (ws: Workspace) =>
    `[project]\ncache_dir = "${ws.path("shared").replaceAll("\\", "/")}"\n`;

folders(["alpha", "beta", "gamma"], {
    files: {
        ...TWO_PROJECTS.files,
        "gamma/main.cpp": "int gamma_fn() { return 0; }\n",
        "alpha/clice.toml": SHARED_CACHE,
        "beta/clice.toml": SHARED_CACHE,
        "gamma/clice.toml": SHARED_CACHE,
    },
    databases: {
        ...TWO_PROJECTS.databases,
        "gamma/compile_commands.json": database({ "gamma/main.cpp": [] }),
    },
})("configured cache directory serves once", async ({ s }) => {
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

const INNER = "outer/inner/main.cpp";

// Listed first, the nested folder still is no project of its own.
folders(["outer/inner", "outer"], {
    files: { [INNER]: gated("IN_OUTER", "inner_fn") },
    databases: { "outer/compile_commands.json": database({ [INNER]: ["-DIN_OUTER"] }) },
})("nested folder joins its project", async ({ s }) => {
    expect(await s.errors(INNER), "the enclosing project compiles it").toEqual([]);
    // One project, holding the client's cache directory: neither folder
    // fell back to a default one.
    expect(s.workspace.exists("outer/.clice")).toBe(false);
    expect(s.workspace.exists("outer/inner/.clice")).toBe(false);

    // Alone, the nested folder is a project that knows no command for it.
    await s.changeFolders({ removed: ["outer"] });
    expect(
        (await pushedErrors(s, INNER))?.length,
        "errors once the nested folder serves alone",
    ).toBeGreaterThan(0);
    await s.changeFolders({ added: ["outer"] });
    expect(await pushedErrors(s, INNER), "the enclosing project to take the folder back").toEqual(
        [],
    );
});

const TOOL = "mono/sub/tool.cpp";
const VENDORED = "mono/sub/vendored.cpp";

folders(["mono"], {
    files: {
        "mono/sub/clice.toml": "",
        [TOOL]: gated("IN_SUB", "tool_fn"),
        [VENDORED]: gated("IN_MONO", "vendored_fn"),
    },
    databases: {
        "mono/compile_commands.json": database({ [VENDORED]: ["-DIN_MONO"] }),
        "mono/sub/build/compile_commands.json": database({ [TOOL]: ["-DIN_SUB"] }),
    },
})("subproject serves what the folder does not build", async ({ s }) => {
    expect(await s.errors(VENDORED), "the folder's build compiles the file it lists").toEqual([]);
    expect(await s.errors(TOOL), "the subproject compiles the rest with its own database").toEqual(
        [],
    );
    expect(await s.errors(VENDORED), "the listed file stays with the folder").toEqual([]);
});

folders(["alpha", "beta"], {
    files: {
        ...TWO_PROJECTS.files,
        "beta/clice.toml": [
            'default_configuration = "fast"',
            "[[rules]]",
            'configuration = "fast"',
            'compile_commands = ["compile_commands.json"]',
            "[[rules]]",
            'configuration = "slow"',
            'compile_commands = ["compile_commands.json"]',
            "",
        ].join("\n"),
    },
    databases: TWO_PROJECTS.databases,
})("configuration menu per project", async ({ s }) => {
    await s.compiled("alpha/main.cpp");
    await s.compiled("beta/main.cpp");
    expect((await s.configurations("alpha/main.cpp")).configurations).toEqual([]);
    expect(await s.configurations("beta/main.cpp")).toMatchObject({
        configurations: ["fast", "slow"],
        active: "fast",
    });
    expect(await s.switchConfiguration("slow", "beta/main.cpp")).toEqual({ success: true });
    expect((await s.configurations("beta/main.cpp")).selected).toBe("slow");
    expect((await s.configurations()).configurations, "the first folder's").toEqual([]);
});

folders(["alpha"], TWO_PROJECTS)("unclaimed file opens its project", async ({ s }) => {
    // Outside every folder, with a database above it: that folder is
    // served as if it were open.
    expect(await s.errors("beta/main.cpp"), "the project found above the file compiles it").toEqual(
        [],
    );
});

folders([], TWO_PROJECTS)("rootless server finds projects", async ({ s }) => {
    expect(await s.errors("alpha/main.cpp")).toEqual([]);
    expect(await s.errors("beta/main.cpp")).toEqual([]);
});

// The case initializes its server itself: a launch's step before the
// initialized notification has no hold of the server it runs on.
serve.files(TWO_PROJECTS.files, {
    databases: TWO_PROJECTS.databases,
    launch: { handshake: false },
})("folder change before initialized", async ({ s }) => {
    await s.client.initialize(s.workspace, {
        folders: ["alpha"],
        beforeInitialized: () => s.changeFolders({ added: ["beta"], removed: ["alpha"] }),
    });
    await s.indexed();
    await listed(s, "beta_fn", BETA_FN);
    expect((await s.workspaceSymbols("alpha_fn")) ?? []).toEqual([]);
});

folders(["alpha"], {
    files: TWO_PROJECTS.files,
    databases: { "alpha/compile_commands.json": ALPHA_DATABASE },
})("added folder adopts its files", async ({ s }) => {
    // Nothing above it knows the file: the first project serves it, without
    // beta's flags.
    expect(
        (await s.errors("beta/main.cpp")).length,
        "no project knows beta's flags yet",
    ).toBeGreaterThan(0);

    s.disk.database(BETA_DATABASE, "beta/compile_commands.json");
    await s.changeFolders({ added: ["beta"] });
    await s.sync();
    expect(
        await s.errors("beta/main.cpp"),
        "the new folder's project compiles the open file",
    ).toEqual([]);
});

folders(["alpha", "beta"], TWO_PROJECTS)("removed folder releases its files", async ({ s }) => {
    expect(await s.errors("beta/main.cpp")).toEqual([]);

    // A moved document recompiles on its own: the client sends nothing
    // that would replace the diagnostics the old project published.
    await s.changeFolders({ removed: ["beta"] });
    expect(
        (await pushedErrors(s, "beta/main.cpp"))?.length,
        "errors from the remaining project, which has no command for it",
    ).toBeGreaterThan(0);

    await s.changeFolders({ added: ["beta"] });
    expect(await pushedErrors(s, "beta/main.cpp"), "the re-added folder to serve it again").toEqual(
        [],
    );
});

folders(["alpha", "beta"], TWO_PROJECTS)(
    "folder re-added at once keeps its cache",
    async ({ s }) => {
        await s.indexed();
        await listed(s, "beta_fn", BETA_FN);

        // The control endpoint record appears only while a project holds the
        // cache directory's writer lock.
        const record = "beta/.clice/server.json";
        expect(s.workspace.exists(record)).toBe(true);
        const before = s.disk.read(record);

        await s.changeFolders({ removed: ["beta"] });
        await s.changeFolders({ added: ["beta"] });
        await s.sync();
        expect(
            s.workspace.exists(record),
            "the re-added folder to take its cache directory back",
        ).toBe(true);
        expect(s.disk.read(record)).not.toBe(before);
    },
);

const SHARED = "alpha/shared.cpp";

folders(["alpha", "beta"], {
    files: { ...TWO_PROJECTS.files, [SHARED]: gated("IN_BETA", "shared_fn") },
    databases: TWO_PROJECTS.databases,
})("database change moves a document", async ({ s }) => {
    expect((await s.errors(SHARED)).length, "no database lists it yet").toBeGreaterThan(0);

    // Beta's database starts listing the file: it moves there.
    s.disk.database(
        database({ "beta/main.cpp": ["-DIN_BETA"], [SHARED]: ["-DIN_BETA"] }),
        "beta/compile_commands.json",
    );
    await s.poll("cdb");
    expect(
        await pushedErrors(s, SHARED),
        "beta to compile the file its database lists now",
    ).toEqual([]);
});

folders(["alpha", "beta"], TWO_PROJECTS)("removed first folder hands over", async ({ s }) => {
    await s.compiled("alpha/main.cpp");
    await s.compiled("beta/main.cpp");

    await s.changeFolders({ removed: ["alpha"] });
    await s.sync();
    expect(
        (await s.errors("alpha/main.cpp")).length,
        "beta serves the file, without alpha's flags",
    ).toBeGreaterThan(0);
    expect(
        await s.hover(at("beta/main.cpp", "int |beta_fn")),
        "beta keeps serving its own",
    ).not.toBeNull();
});

folders(["alpha"], TWO_PROJECTS)("removed only folder goes rootless", async ({ s }) => {
    expect(await s.errors("alpha/main.cpp")).toEqual([]);

    await s.changeFolders({ removed: ["alpha"] });
    await s.sync();
    expect(
        (await s.errors("alpha/main.cpp")).length,
        "the rootless project guesses a command",
    ).toBeGreaterThan(0);
});

folders(["app", "lib"], LIBRARY_AND_APP)("definition crosses folders", async ({ s }) => {
    // The application's index only declares lib_fn; the library's defines
    // it, once its background index lands.
    await s.compiled("app/main.cpp");
    await s.indexed();
    expect(s.show(await s.definition(LIB_FN_CALL)).split("\n")).toContain(LIB_FN);
});

folders(["app", "lib"], LIBRARY_AND_APP)("references cross folders", async ({ s }) => {
    await s.compiled("lib/src/lib.cpp");
    await s.indexed();
    await listed(s, "main", MAIN);
    expect(s.show(await s.references(at("lib/src/lib.cpp", "int l|ib_fn"))).split("\n")).toContain(
        MAIN,
    );
});

// The macro's id takes its header relative to the library's root in the
// library and absolute in the application; the two meet at its definition.
folders(["app", "lib"], {
    files: {
        ...LIBRARY_AND_APP.files,
        "lib/include/lib.h": "#pragma once\n#define LIB_LIMIT 4\nint lib_fn();\n",
        "lib/src/lib.cpp": '#include "lib.h"\nint lib_fn() { return LIB_LIMIT; }\n',
        "app/main.cpp": '#include "lib.h"\nint main() { return LIB_LIMIT + lib_fn(); }\n',
    },
    databases: LIBRARY_AND_APP.databases,
})("shared macro references cross folders", async ({ s }) => {
    await s.compiled("lib/src/lib.cpp");
    await s.indexed();
    await listed(s, "main", "app/main.cpp: int main() { return LIB_LIMIT + lib_fn(); }");
    expect(s.show(await s.references(at("lib/src/lib.cpp", "L|IB_LIMIT"))).split("\n")).toContain(
        "app/main.cpp: int main() { return LIB_LIMIT + lib_fn(); }",
    );
});

folders(["app", "lib"], {
    files: {
        ...LIBRARY_AND_APP.files,
        "lib/include/shape.h":
            "#pragma once\nstruct Shape {\n    virtual int area() const = 0;\n};\n",
        "lib/src/lib.cpp":
            '#include "lib.h"\n#include "shape.h"\nint lib_fn() { return shared_fn(); }\n',
        "app/main.cpp":
            '#include "lib.h"\n#include "shape.h"\n' +
            "struct Square : Shape {\n    int area() const override { return 4; }\n};\n" +
            "int main() { return lib_fn(); }\n",
    },
    databases: LIBRARY_AND_APP.databases,
})("hierarchies cross folders", async ({ s }) => {
    await s.indexed();
    await listed(s, "main", MAIN);
    await listed(s, "lib_fn", LIB_FN);

    // The library's function is called from the application only.
    const fn = at("lib/src/lib.cpp", "int l|ib_fn");
    await s.compiled("lib/src/lib.cpp");
    const [called] =
        (await s.request<proto.CallHierarchyItem[] | null>(
            "textDocument/prepareCallHierarchy",
            fn,
        )) ?? [];
    expect(called?.name).toBe("lib_fn");
    expect((await s.incomingCalls(fn))?.map((call) => call.from.name)).toEqual(["main"]);

    // The library's interface is implemented in the application only.
    const shape = at("lib/include/shape.h", "struct S|hape");
    await s.compiled("lib/include/shape.h");
    const [base] =
        (await s.request<proto.TypeHierarchyItem[] | null>(
            "textDocument/prepareTypeHierarchy",
            shape,
        )) ?? [];
    expect(base?.name).toBe("Shape");
    expect((await s.subtypes(shape))?.map((type) => type.name)).toEqual(["Square"]);
    expect(s.show(await s.request("textDocument/implementation", shape))).toBe(
        "app/main.cpp: struct Square : Shape {",
    );

    // The other way round the library knows nothing of the application's
    // type, yet its base stays reachable though the library serves the
    // base's open file.
    const square = at("app/main.cpp", "struct S|quare");
    await s.compiled("app/main.cpp");
    const [derived] =
        (await s.request<proto.TypeHierarchyItem[] | null>(
            "textDocument/prepareTypeHierarchy",
            square,
        )) ?? [];
    expect(derived?.name).toBe("Square");
    expect((await s.supertypes(square))?.map((type) => type.name)).toEqual(["Shape"]);
});

// The application builds the library's source too; the library, listed
// first, serves it once open.
folders(["lib", "app"], {
    files: LIBRARY_AND_APP.files,
    databases: {
        ...LIBRARY_AND_APP.databases,
        "app/compile_commands.json": database({
            "app/main.cpp": [INCLUDE_LIB],
            "lib/src/lib.cpp": [INCLUDE_LIB],
        }),
    },
})("definition follows an open buffer", async ({ s }) => {
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

folders(["app", "lib"], LIBRARY_AND_APP)(
    "an open file answers through its project",
    async ({ s }) => {
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
        expect(into(s, await s.references(LIB_FN_CALL), header)).toEqual([
            `${header}: int lib_fn();`,
        ]);
    },
);

// The dependency folder has no database: its header compiles in the
// context of the application including it.
folders(["app", "dep"], {
    files: {
        "dep/include/dep.h": "#pragma once\n" + gated("IN_APP", "dep_fn"),
        "app/main.cpp": '#include "dep.h"\nint main() { return dep_fn(); }\n',
    },
    databases: {
        "app/compile_commands.json": database({
            "app/main.cpp": ["-I${workspace}/dep/include", "-DIN_APP"],
        }),
    },
})("dependency folder borrows the application", async ({ s }) => {
    expect(
        await s.errors("dep/include/dep.h"),
        "the application's command reaches the header",
    ).toEqual([]);
});

folders(["app", "lib"], {
    files: {
        ...LIBRARY_AND_APP.files,
        "lib/include/lib.h": "#pragma once\n" + gated("IN_APP", "lib_fn"),
    },
    databases: {
        ...LIBRARY_AND_APP.databases,
        "app/compile_commands.json": database({ "app/main.cpp": [INCLUDE_LIB, "-DIN_APP"] }),
    },
})("context from another folder", async ({ s }) => {
    const header = "lib/include/lib.h";

    // The library's own source is the header's host at first; the
    // application's is on offer too.
    expect((await s.errors(header)).length, "the library does not define IN_APP").toBeGreaterThan(
        0,
    );
    const listed = await s.contexts(header);
    const hosts = listed.contexts.map((context) => context.uri);
    expect(hosts).toContain(s.uri("lib/src/lib.cpp"));
    expect(hosts).toContain(s.uri("app/main.cpp"));

    const switched = await s.switchContext(header, "app/main.cpp", { epoch: listed.epoch });
    expect(switched.success).toBe(true);
    expect(await s.errors(header), "the application's host defines IN_APP").toEqual([]);
    expect((await s.currentContext(header)).context?.uri).toBe(s.uri("app/main.cpp"));

    // The reset hands the header back to its own folder's host; the
    // application's project retracts its diagnostics first.
    expect((await s.resetContext(header)).success).toBe(true);
    expect(
        (await pushedErrors(s, header))?.length,
        "the library does not define IN_APP",
    ).toBeGreaterThan(0);
    const current = await s.currentContext(header);
    expect(current.automatic).toBe(true);
    expect(current.context?.uri).toBe(s.uri("lib/src/lib.cpp"));
});

folders(["alpha", "beta"], SHARED_FILE)("own configuration of another folder", async ({ s }) => {
    // Both databases list the file: its owner's entry comes first, the
    // other project's can be switched to, moving the file there.
    await s.compiled(SHARED);
    const listed = await s.contexts(SHARED);
    const own = listed.contexts.filter((context) => context.uri === s.uri(SHARED));
    expect(own).toHaveLength(2);
    const other = own[1]!.commandHash!;
    const switched = await s.switchContext(SHARED, SHARED, {
        commandHash: other,
        epoch: listed.epoch,
    });
    expect(switched.success).toBe(true);
    expect((await s.currentContext(SHARED)).context?.commandHash).toBe(other);
});

folders(["alpha", "beta"], SHARED_FILE)("a closed file keeps its choice", async ({ s }) => {
    await s.compiled(SHARED);
    const listed = await s.contexts(SHARED);
    const own = listed.contexts.filter((context) => context.uri === s.uri(SHARED));
    expect(own).toHaveLength(2);
    const first = own[0]!.commandHash!;
    const other = own[1]!.commandHash!;
    const switched = await s.switchContext(SHARED, SHARED, {
        commandHash: other,
        epoch: listed.epoch,
    });
    expect(switched.success).toBe(true);

    s.close(SHARED);
    const refused = await s.switchContext(SHARED, SHARED, { commandHash: first });
    expect(refused.success).toBe(false);
    await s.compiled(SHARED);
    expect((await s.currentContext(SHARED)).context?.commandHash).toBe(other);
});

folders(["app", "lib"], LIBRARY_AND_APP)("save reaches every folder", async ({ s }) => {
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

const HELPER = (user: string) => `int helper() { return 0; }\nint ${user}() { return helper(); }\n`;

folders(["alpha", "beta"], {
    files: { "alpha/main.cpp": HELPER("use_alpha"), "beta/main.cpp": HELPER("use_beta") },
    databases: {
        "alpha/compile_commands.json": database({ "alpha/main.cpp": [] }),
        "beta/compile_commands.json": database({ "beta/main.cpp": [] }),
    },
})("unrelated folders keep references apart", async ({ s }) => {
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

folders(["alpha", "beta"], TWO_PROJECTS)("batch index asks the folder's server", async ({ s }) => {
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

folders(["alpha", "empty", "beta"], TWO_PROJECTS, {
    setup: (ws) => {
        ws.mkdir("empty");
    },
})("indexing progress ends across folders", async ({ s }) => {
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

folders(["alpha", "beta"], TWO_PROJECTS)("restart serves every folder", async ({ s }) => {
    await s.indexed();
    await listed(s, "alpha_fn", ALPHA_FN);
    await listed(s, "beta_fn", BETA_FN);

    // Both folders load their persisted index at startup: the first query
    // answers before any worker could have reindexed either.
    await s.restart();
    const symbols = (await s.workspaceSymbols("_fn")) ?? [];
    expect(symbols.map((symbol) => symbol.name).sort()).toEqual(["alpha_fn", "beta_fn"]);
    await s.noAnomaly();
});
