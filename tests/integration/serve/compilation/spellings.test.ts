/// How the build spells a path decides where a lookup starts: a quoted
/// include from the directory its includer was reached through, `..` past a
/// symlinked directory. Results name a file the way the user opened it —
/// its document, else its own path under the workspace folder — never the
/// way a lookup reached it.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { asLocations } from "@clice/tools/client";
import { Workspace } from "@clice/tools/workspace";
import { at, expect, serve } from "../../fixtures.ts";

const windows = process.platform === "win32";

serve
    .files(
        {
            "real/main.cpp": '#include "config.h"\nint main() { return VALUE; }\n',
            "src/config.h": "#define VALUE 0\n",
        },
        {
            manifest: { units: { "src/main.cpp": [] } },
            setup: (workspace) => {
                fs.symlinkSync(workspace.path("real/main.cpp"), workspace.path("src/main.cpp"));
            },
        },
    )
    .skipIf(windows)("symlinked source includes beside the link", async ({ s }) => {
    expect(await s.errors("src/main.cpp"), "clang finds config.h beside the link").toEqual([]);
    const hosts = await s.contexts("src/config.h");
    expect(hosts.total, "the header has the source as its host").toBe(1);
});

serve
    .files(
        {
            "vendor/real/api.h": "#include <../common.h>\n",
            "vendor/common.h": "int common();\n",
            "main.cpp": "#include <api.h>\nint main() { return common(); }\n",
        },
        {
            manifest: { args: ["-Iinc"], units: { "main.cpp": [] } },
            setup: (workspace) => {
                fs.symlinkSync(workspace.path("vendor/real"), workspace.path("inc"));
            },
        },
    )
    .skipIf(windows)("parent segment past a symlinked directory", async ({ s }) => {
    expect(await s.errors("main.cpp"), "`..` climbs from the link's target").toEqual([]);
    const hosts = await s.contexts("vendor/common.h");
    expect(hosts.total, "the header clang includes has a host").toBe(1);
});

serve
    .files(
        {
            "vendor/real/utils.h": "inline int get_x(int p) { return p; }\n",
            "main.cpp": '#include "utils.h"\nint main() { return get_x(1); }\n',
        },
        {
            manifest: { args: ["-Iinc"], units: { "main.cpp": [] } },
            setup: (workspace) => {
                fs.symlinkSync(workspace.path("vendor/real"), workspace.path("inc"));
            },
        },
    )
    .skipIf(windows)("header found through a symlinked directory", async ({ s }) => {
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

serve
    .files(
        {
            "inc/early.h": "int early();\n",
            "inc/late.h": "int late();\n",
            "main.cpp":
                '#include "early.h"\nint first();\n#include "late.h"\nint main() { return early() + late(); }\n',
        },
        { databases: false, launch: { handshake: false } },
    )
    .skipIf(windows)("folder opened through a symlink", async ({ s }) => {
    const outer = s.session.tmpdir();
    fs.symlinkSync(s.workspace.root, outer.path("ws"));
    const folder = new Workspace(outer.path("ws"));
    folder.writeCDB(["main.cpp"], { extraArgs: ["-Iinc"] });
    await s.client.initialize(folder);

    const main = folder.path("main.cpp");
    await s.compiled(main);
    const early = asLocations(await s.definition(at(main, '"ea|rly.h"')));
    expect(early.map((location) => location.uri)).toEqual([folder.uri("inc/early.h")]);
    const late = asLocations(await s.definition(at(main, '"la|te.h"')));
    expect(
        late.map((location) => location.uri),
        "past the preamble",
    ).toEqual([folder.uri("inc/late.h")]);
    const hover = await s.hover(at(main, '"la|te.h"'));
    expect(JSON.stringify(hover?.contents)).toContain(folder.path("inc/late.h"));
});

serve.files(
    {
        "include/lib.h": "int lib_fn(int);\n",
        "common/c.h": "inline int common_fn() { return 1; }\n",
        "src/main.cpp":
            '#include "lib.h"\n#include "../common/c.h"\nint main() { return lib_fn(common_fn()); }\n',
        "build/compile_commands.json": (workspace) =>
            JSON.stringify([
                {
                    directory: workspace.path("build"),
                    file: "../src/main.cpp",
                    arguments: ["clang++", "-std=c++20", "-I../include", "-c", "../src/main.cpp"],
                },
            ]),
    },
    { databases: false },
)("build that climbs out of its directory", async ({ s }) => {
    await s.compiled("src/main.cpp");
    const links = await s.request<proto.DocumentLink[] | null>(
        "textDocument/documentLink",
        "src/main.cpp",
    );
    expect((links ?? []).map((link) => link.target)).toEqual([
        s.uri("include/lib.h"),
        s.uri("common/c.h"),
    ]);
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
    { manifest: { args: ["-Iinc"], units: { "main.cpp": [] } }, launch: { handshake: false } },
)("folder spelled with a trailing slash", async ({ s }) => {
    await s.client.initialize(new Workspace(s.workspace.root + "/"));

    await s.compiled("main.cpp");
    const links = await s.request<proto.DocumentLink[] | null>(
        "textDocument/documentLink",
        "main.cpp",
    );
    expect((links ?? []).map((link) => link.target)).toEqual([s.uri("inc/h.h")]);
    const definition = asLocations(await s.definition(at("main.cpp", "return |f()")));
    expect(definition.map((location) => location.uri)).toEqual([s.uri("inc/h.h")]);
});

serve("tiny")("document without a file is not served", async ({ s }) => {
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
});
