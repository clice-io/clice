/// Files saved as "UTF-8 with BOM": every part of the server sees their text
/// without the mark, as an editor shows and sends it.

import * as proto from "vscode-languageserver-protocol";
import { expect, test } from "../fixtures.ts";

const BOM = "\uFEFF";

function startOf(location: proto.Location | proto.LocationLink | undefined): string | null {
    if (location === undefined) {
        return null;
    }
    const range = "targetUri" in location ? location.targetSelectionRange : location.range;
    return `${range.start.line}:${range.start.character}`;
}

test("first line positions skip the mark", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("a.h", `${BOM}int shared_fn();\n`);
    workspace.write("c.cpp", `${BOM}int user3() { return 3; }\n#include "a.h"\n`);
    workspace.write("main.cpp", `#include "a.h"\nint use() { return shared_fn(); }\n`);
    workspace.writeCDB(["c.cpp", "main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "user3")).toBe(true);

    const symbol = ((await client.workspaceSymbols("user3")) ?? []).find((s) => s.name === "user3");
    expect(symbol && "range" in symbol.location ? startOf(symbol.location) : null).toBe("0:4");
    const definition = await client.definitionAt(uri, 1, "int use() { return ".length + 1);
    expect(startOf(Array.isArray(definition) ? definition[0] : (definition ?? undefined))).toBe(
        "0:4",
    );
});

test("a marked host keeps the header context", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write("utils.h", "inline int get_x(Point p) { return p.x; }\n");
    workspace.write(
        "main.cpp",
        `${BOM}#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n`,
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [utilsUri] = await client.openAndWait("utils.h");
    client.assertCleanCompile(utilsUri);
});
