/// Fragments compiled in their includer's context: a class body or an
/// X-macro list sits inside a declaration the includer opens before the
/// include and closes after it.

import * as path from "node:path";
import { runProcess } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import type * as proto from "vscode-languageserver-protocol";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const BODY = "int field_a;\nint field_b;\nint sum() const { return field_a + field_b; }\n";

function writeBody(workspace: Workspace) {
    workspace.write("body.inc", BODY);
    workspace.write(
        "rec.cpp",
        'struct Rec {\n#include "body.inc"\n};\nint main() { Rec r; return r.sum(); }\n',
    );
    workspace.writeCDB(["rec.cpp"]);
}

function names(symbols: proto.DocumentSymbol[] | proto.SymbolInformation[] | null): string[] {
    return (symbols ?? []).map((symbol) => symbol.name);
}

test("class body fragment", async ({ session }) => {
    const { client, workspace } = session.tmp();
    writeBody(workspace);
    await client.initialize(workspace);

    const [bodyUri] = await client.openAndWait("body.inc");
    client.assertCleanCompile(bodyUri);
    expect(names(await client.documentSymbols(bodyUri))).toEqual(["field_a", "field_b", "sum"]);
    expect(JSON.stringify((await client.hoverAt(bodyUri, 2, 26))?.contents)).toContain(
        "field `field_a`",
    );
    expect(await client.definitionAt(bodyUri, 2, 26)).toEqual([
        {
            uri: bodyUri,
            range: { start: { line: 0, character: 4 }, end: { line: 0, character: 11 } },
        },
    ]);
});

test("enumerator list fragment", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("errors.def", "ERROR(NotFound, 404)\nERROR(Forbidden, 403)\n");
    workspace.write(
        "codes.cpp",
        "enum class Code {\n#define ERROR(name, value) name = value,\n" +
            '#include "errors.def"\n#undef ERROR\n};\n' +
            "const char* text(Code c) {\n  switch (c) {\n" +
            "#define ERROR(name, value) case Code::name: return #name;\n" +
            '#include "errors.def"\n#undef ERROR\n  }\n  return "";\n}\n',
    );
    workspace.writeCDB(["codes.cpp"]);
    await client.initialize(workspace);

    const [defUri] = await client.openAndWait("errors.def");
    client.assertCleanCompile(defUri);
    expect(names(await client.documentSymbols(defUri))).toEqual(["NotFound", "Forbidden"]);
    expect(JSON.stringify((await client.hoverAt(defUri, 0, 8))?.contents)).toContain(
        "enumerator `NotFound`",
    );
});

test("inspect closes the fragment", async ({ session }) => {
    const workspace = session.tmpdir();
    writeBody(workspace);

    const run = await runProcess(
        cliceExecutable(),
        ["inspect", "document_symbol", path.join(workspace.root, "body.inc")],
        { timeout: 120_000 },
    );
    expect(run.status, run.stderr).toBe(0);
    const output = JSON.parse(run.stdout) as {
        files: Record<string, { result: { name: string }[]; diagnostics: string[] | null }>;
    };
    const body = output.files["body.inc"];
    expect(body?.diagnostics ?? []).toEqual([]);
    expect(body?.result.map((symbol) => symbol.name)).toEqual(["field_a", "field_b", "sum"]);
});
