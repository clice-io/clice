/// How the build spells a path decides where a lookup starts: a quoted
/// include from the directory its includer was reached through, `..` past a
/// symlinked directory. Results name a file the way the user opened it —
/// its document, else its own path under the workspace folder — never the
/// way a lookup reached it.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { describe } from "vitest";
import { asLocations } from "@clice/tools/client";
import { writeDatabase } from "@clice/tools/project";
import { Workspace } from "@clice/tools/workspace";
import { at, expect, serve } from "../../fixtures.ts";

/// The fixture's server starts on no unit: a case lays its links and its
/// database down while no server runs.
const OWN_DATABASE = { manifest: { units: {} } };

describe.skipIf(process.platform === "win32")("posix", () => {
    serve.files(
        {
            "real/main.cpp": '#include "config.h"\nint main() { return VALUE; }\n',
            "src/config.h": "#define VALUE 0\n",
        },
        OWN_DATABASE,
    )("symlinked source includes beside the link", async ({ s }) => {
        await s.offline(() => {
            fs.symlinkSync(s.workspace.path("real/main.cpp"), s.workspace.path("src/main.cpp"));
            writeDatabase(s.workspace, { units: { "src/main.cpp": [] } });
        });
        expect(await s.errors("src/main.cpp"), "clang finds config.h beside the link").toEqual([]);
        const hosts = await s.client.queryContext(s.uri("src/config.h"));
        expect(hosts.total, "the header has the source as its host").toBe(1);
    });

    serve.files(
        {
            "vendor/real/api.h": "#include <../common.h>\n",
            "vendor/common.h": "int common();\n",
            "main.cpp": "#include <api.h>\nint main() { return common(); }\n",
        },
        OWN_DATABASE,
    )("parent segment past a symlinked directory", async ({ s }) => {
        await s.offline(() => {
            fs.symlinkSync(s.workspace.path("vendor/real"), s.workspace.path("inc"));
            writeDatabase(s.workspace, { args: ["-Iinc"], units: { "main.cpp": [] } });
        });
        expect(await s.errors("main.cpp"), "`..` climbs from the link's target").toEqual([]);
        const hosts = await s.client.queryContext(s.uri("vendor/common.h"));
        expect(hosts.total, "the header clang includes has a host").toBe(1);
    });

    serve.files(
        {
            "vendor/real/utils.h": "inline int get_x(int p) { return p; }\n",
            "main.cpp": '#include "utils.h"\nint main() { return get_x(1); }\n',
        },
        OWN_DATABASE,
    )("header found through a symlinked directory", async ({ s }) => {
        await s.offline(() => {
            fs.symlinkSync(s.workspace.path("vendor/real"), s.workspace.path("inc"));
            writeDatabase(s.workspace, { args: ["-Iinc"], units: { "main.cpp": [] } });
        });
        await s.compiled("main.cpp");
        const targets = asLocations(await s.definition(at("main.cpp", "g|et_x(1)")));
        expect(
            targets.map((target) => target.uri),
            "named by its own path",
        ).toEqual([s.uri("vendor/real/utils.h")]);
        await s.compiled("inc/utils.h");
        expect(
            await s.hover(at("inc/utils.h", "g|et_x")),
            "the lookup's name is served",
        ).not.toBeNull();
    });

    serve.files({}, OWN_DATABASE)("folder opened through a symlink", async ({ s }) => {
        // The server opens the folder by another spelling than the one the
        // case's server runs on.
        await s.stop();
        const outer = s.session.tmpdir();
        fs.symlinkSync(s.workspace.root, outer.path("ws"));
        const workspace = new Workspace(outer.path("ws"));
        workspace.write("inc/early.h", "int early();\n");
        workspace.write("inc/late.h", "int late();\n");
        workspace.write(
            "main.cpp",
            '#include "early.h"\nint first();\n#include "late.h"\nint main() { return early() + late(); }\n',
        );
        workspace.writeCDB(["main.cpp"], { extraArgs: ["-Iinc"] });
        const client = await s.session.spawn(workspace).initialize(workspace);

        const [main] = await client.openAndWait("main.cpp");
        expect(await client.definitionUris(main, 0, 12)).toEqual([workspace.uri("inc/early.h")]);
        expect(await client.definitionUris(main, 2, 12), "past the preamble").toEqual([
            workspace.uri("inc/late.h"),
        ]);
        const hover = await client.hoverAt(main, 2, 12);
        expect(JSON.stringify(hover?.contents)).toContain(workspace.path("inc/late.h"));
    });
});

serve.files(
    {
        "include/lib.h": "int lib_fn(int);\n",
        "common/c.h": "inline int common_fn() { return 1; }\n",
        "src/main.cpp":
            '#include "lib.h"\n#include "../common/c.h"\nint main() { return lib_fn(common_fn()); }\n',
    },
    OWN_DATABASE,
)("build that climbs out of its directory", async ({ s }) => {
    // The database sits in build/, which the fixture writes none in.
    await s.offline(() => {
        s.disk.rm("compile_commands.json");
        s.disk.write(
            "build/compile_commands.json",
            JSON.stringify([
                {
                    directory: s.workspace.path("build"),
                    file: "../src/main.cpp",
                    arguments: ["clang++", "-std=c++20", "-I../include", "-c", "../src/main.cpp"],
                },
            ]),
        );
    });

    await s.compiled("src/main.cpp");
    const links = ((await s.request("textDocument/documentLink", "src/main.cpp")) ??
        []) as proto.DocumentLink[];
    expect(links.map((link) => link.target)).toEqual([s.uri("include/lib.h"), s.uri("common/c.h")]);
    const definition = asLocations(await s.definition(at("src/main.cpp", "lib|_fn(common")));
    expect(definition.map((location) => location.uri)).toEqual([s.uri("include/lib.h")]);
    await s.indexed();
    const symbols = (await s.workspaceSymbols("common_fn")) ?? [];
    expect(symbols.some((symbol) => symbol.name === "common_fn")).toBe(true);
    expect(symbols.map((symbol) => ("location" in symbol ? symbol.location.uri : ""))).toEqual([
        s.uri("common/c.h"),
    ]);
});

serve.files(
    { "inc/h.h": "int f();\n", "main.cpp": '#include "h.h"\nint g() { return f(); }\n' },
    OWN_DATABASE,
)("folder spelled with a trailing slash", async ({ s }) => {
    // The server opens the folder by another spelling than the one the
    // case's server runs on.
    await s.stop();
    writeDatabase(s.workspace, { args: ["-Iinc"], units: { "main.cpp": [] } });
    const client = await s.session
        .spawn(s.workspace)
        .initialize(new Workspace(s.workspace.root + "/"));

    const [main] = await client.openAndWait("main.cpp");
    const links = ((await client.documentLinks(main)) ?? []).map((link) => link.target);
    expect(links).toEqual([s.uri("inc/h.h")]);
    expect(await client.definitionUris(main, 1, 17)).toEqual([s.uri("inc/h.h")]);
});

serve.files({ "main.cpp": "int main() { return 0; }\n" })(
    "document without a file is not served",
    async ({ s }) => {
        const untitled = "untitled:Untitled-1";
        await s.client.sendNotification(proto.DidOpenTextDocumentNotification.type, {
            textDocument: {
                uri: untitled,
                languageId: "cpp",
                version: 0,
                text: "int main() {}\n",
            },
        });
        await expect(s.client.hoverAt(untitled, 0, 5)).rejects.toThrow("Document not open");
        expect(await s.client.referencesAt(untitled, 0, 5), "no place in the index").toEqual([]);
        expect(await s.errors("main.cpp"), "the server keeps serving files").toEqual([]);
    },
);
