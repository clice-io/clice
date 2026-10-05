/// How the build spells a path decides where a lookup starts: a quoted
/// include from the directory its includer was reached through, `..` past a
/// symlinked directory. Results name a file the way the user opened it —
/// its document, else its own path under the workspace folder — never the
/// way a lookup reached it.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

const posix = test.skipIf(process.platform === "win32");

posix("symlinked source includes beside the link", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("real/main.cpp", '#include "config.h"\nint main() { return VALUE; }\n');
    workspace.write("src/config.h", "#define VALUE 0\n");
    fs.symlinkSync(workspace.path("real/main.cpp"), workspace.path("src/main.cpp"));
    workspace.writeCDB(["src/main.cpp"]);
    await client.initialize(workspace);

    const [main] = await client.openAndWait("src/main.cpp");
    client.assertNoErrors(main, "clang finds config.h beside the link");
    const hosts = await client.queryContext(workspace.uri("src/config.h"));
    expect(hosts.total, "the header has the source as its host").toBe(1);
});

posix("parent segment past a symlinked directory", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("vendor/real/api.h", "#include <../common.h>\n");
    workspace.write("vendor/common.h", "int common();\n");
    fs.symlinkSync(workspace.path("vendor/real"), workspace.path("inc"));
    workspace.write("main.cpp", "#include <api.h>\nint main() { return common(); }\n");
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-Iinc"] });
    await client.initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    client.assertNoErrors(main, "`..` climbs from the link's target");
    const hosts = await client.queryContext(workspace.uri("vendor/common.h"));
    expect(hosts.total, "the header clang includes has a host").toBe(1);
});

posix("header found through a symlinked directory", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("vendor/real/utils.h", "inline int get_x(int p) { return p; }\n");
    fs.symlinkSync(workspace.path("vendor/real"), workspace.path("inc"));
    workspace.write("main.cpp", '#include "utils.h"\nint main() { return get_x(1); }\n');
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-Iinc"] });
    await client.initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    const targets = await client.definitionUris(main, 1, 21);
    expect(targets, "named by its own path").toEqual([workspace.uri("vendor/real/utils.h")]);
    const [header] = await client.openAndWait("inc/utils.h");
    expect(await client.hoverAt(header, 0, 12), "the lookup's name is served").not.toBeNull();
});

test("build that climbs out of its directory", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("include/lib.h", "int lib_fn(int);\n");
    workspace.write("common/c.h", "inline int common_fn() { return 1; }\n");
    workspace.write(
        "src/main.cpp",
        '#include "lib.h"\n#include "../common/c.h"\nint main() { return lib_fn(common_fn()); }\n',
    );
    workspace.write(
        "build/compile_commands.json",
        JSON.stringify([
            {
                directory: workspace.path("build"),
                file: "../src/main.cpp",
                arguments: ["clang++", "-std=c++20", "-I../include", "-c", "../src/main.cpp"],
            },
        ]),
    );
    await client.initialize(workspace);

    const [main] = await client.openAndWait("src/main.cpp");
    const links = ((await client.documentLinks(main)) ?? []).map((link) => link.target);
    expect(links).toEqual([workspace.uri("include/lib.h"), workspace.uri("common/c.h")]);
    expect(await client.definitionUris(main, 2, 23)).toEqual([workspace.uri("include/lib.h")]);
    expect(await client.waitForIndex(main, "common_fn")).toBe(true);
    const symbols = (await client.workspaceSymbols("common_fn")) ?? [];
    expect(symbols.map((symbol) => ("location" in symbol ? symbol.location.uri : ""))).toEqual([
        workspace.uri("common/c.h"),
    ]);
});

posix("folder opened through a symlink", async ({ session }) => {
    const real = session.tmpdir();
    const outer = session.tmpdir();
    fs.symlinkSync(real.root, outer.path("ws"));
    const workspace = new Workspace(outer.path("ws"));
    workspace.write("inc/early.h", "int early();\n");
    workspace.write("inc/late.h", "int late();\n");
    workspace.write(
        "main.cpp",
        '#include "early.h"\nint first();\n#include "late.h"\nint main() { return early() + late(); }\n',
    );
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-Iinc"] });
    const client = await session.spawn(workspace).initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    expect(await client.definitionUris(main, 0, 12)).toEqual([workspace.uri("inc/early.h")]);
    expect(await client.definitionUris(main, 2, 12), "past the preamble").toEqual([
        workspace.uri("inc/late.h"),
    ]);
    const hover = await client.hoverAt(main, 2, 12);
    expect(JSON.stringify(hover?.contents)).toContain(workspace.path("inc/late.h"));
});

test("folder spelled with a trailing slash", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("inc/h.h", "int f();\n");
    workspace.write("main.cpp", '#include "h.h"\nint g() { return f(); }\n');
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-Iinc"] });
    await client.initialize(new Workspace(workspace.root + "/"));

    const [main] = await client.openAndWait("main.cpp");
    const links = ((await client.documentLinks(main)) ?? []).map((link) => link.target);
    expect(links).toEqual([workspace.uri("inc/h.h")]);
    expect(await client.definitionUris(main, 1, 17)).toEqual([workspace.uri("inc/h.h")]);
});

test("document without a file is not served", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const untitled = "untitled:Untitled-1";
    await client.sendNotification(proto.DidOpenTextDocumentNotification.type, {
        textDocument: { uri: untitled, languageId: "cpp", version: 0, text: "int main() {}\n" },
    });
    await expect(client.hoverAt(untitled, 0, 5)).rejects.toThrow("Document not open");
    expect(await client.referencesAt(untitled, 0, 5), "no place in the index").toEqual([]);
    const [main] = await client.openAndWait("main.cpp");
    client.assertNoErrors(main, "the server keeps serving files");
});
