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
    workspace.write("base.h", "#pragma once\nstruct Base {};\n");
    workspace.write(
        "rec.cpp",
        '#include "base.h"\nstruct Rec : Base {\n#include "body.inc"\n};\n' +
            "int main() { Rec r; return r.sum(); }\n",
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
    // What precedes the class the fragment sits in is a preamble.
    expect(workspace.pchFiles()).toHaveLength(1);
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

test("tcc fragment", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("body.tcc", "public:\nint x;\n");
    workspace.write("s.cpp", 'struct S {\n#include "body.tcc"\n};\nint main() { return S{}.x; }\n');
    workspace.writeCDB(["s.cpp"]);
    await client.initialize(workspace);

    const [tccUri] = await client.openAndWait("body.tcc");
    client.assertCleanCompile(tccUri);
});

test("unclosed host scope", async ({ session }) => {
    // The fragment leaves its struct open: the includer's brace closes it,
    // and the includer's namespace stays open to the end of the unit.
    const { client, workspace } = session.tmp();
    workspace.write("x.inc", "struct S {\nint a;\n");
    workspace.write("lib.cpp", 'namespace lib {\n#include "x.inc"\n}\nint main() { return 0; }\n');
    workspace.writeCDB(["lib.cpp"]);
    await client.initialize(workspace);

    const [incUri] = await client.openAndWait("x.inc");
    expect(names(await client.documentSymbols(incUri))).toEqual(["S"]);
});

test("def entered twice", async ({ session }) => {
    // The header's enum enters the list first; the source's table enters
    // it again after including the header.
    const { client, workspace } = session.tmp();
    workspace.write("ops.def", "OP(Add)\nOP(Sub)\n");
    workspace.write(
        "ops.h",
        '#pragma once\nenum Op {\n#define OP(x) x,\n#include "ops.def"\n#undef OP\n};\n',
    );
    workspace.write(
        "ops.cpp",
        '#include "ops.h"\nconst char* names[] = {\n#define OP(x) #x,\n#include "ops.def"\n' +
            "#undef OP\n};\nint main() { return Add; }\n",
    );
    workspace.writeCDB(["ops.cpp"]);
    await client.initialize(workspace);

    const [defUri] = await client.openAndWait("ops.def");
    client.assertCleanCompile(defUri);
    expect(names(await client.documentSymbols(defUri))).toEqual(["Add", "Sub"]);

    const switched = await client.switchContext(defUri, workspace.uri("ops.cpp"), {
        occurrence: 1,
    });
    expect(switched.success).toBe(true);
    await client.waitForRecompile(defUri);
    client.assertCleanCompile(defUri);
});

test("host macro definition", async ({ session }) => {
    // The macro the includer defines before the include point resolves
    // to its definition there.
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "ITEM(alpha)\n");
    workspace.write(
        "main.cpp",
        '#define ITEM(name) int name;\n#include "list.def"\n#undef ITEM\nint main() { return alpha; }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = client.open("main.cpp");
    expect(await client.waitForIndex(mainUri, "main")).toBe(true);
    const [defUri] = await client.openAndWait("list.def");
    expect(await client.definitionAt(defUri, 0, 1)).toEqual([
        {
            uri: mainUri,
            range: { start: { line: 0, character: 8 }, end: { line: 0, character: 12 } },
        },
    ]);
});
