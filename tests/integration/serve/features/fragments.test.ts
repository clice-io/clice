/// Fragments compiled in their includer's context: a class body or an
/// X-macro list sits inside a declaration the includer opens before the
/// include and closes after it.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve, ServeOptions } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

/// Every source a C++17 unit.
function cxx17(sources: string[]): ServeOptions {
    return {
        manifest: {
            cxx: ["-std=c++17"],
            units: Object.fromEntries(sources.map((source) => [source, []])),
        },
    };
}

async function names(s: Serve, file: string): Promise<string[]> {
    const symbols = (await s.request("textDocument/documentSymbol", file)) as
        | proto.DocumentSymbol[]
        | proto.SymbolInformation[]
        | null;
    return (symbols ?? []).map((symbol) => symbol.name);
}

async function indexes(s: Serve, name: string): Promise<boolean> {
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    return symbols.some((symbol) => symbol.name === name);
}

serve.files(
    {
        "body.inc": "int field_a;\nint field_b;\nint sum() const { return field_a + field_b; }\n",
        "base.h": "#pragma once\nstruct Base {};\n",
        "rec.cpp":
            '#include "base.h"\nstruct Rec : Base {\n#include "body.inc"\n};\n' +
            "int main() { Rec r; return r.sum(); }\n",
    },
    cxx17(["rec.cpp"]),
)("class body fragment", async ({ s }) => {
    await s.clean("body.inc");
    // What precedes the class the fragment sits in is a preamble.
    expect(s.workspace.pchFiles()).toHaveLength(1);
    expect(await names(s, "body.inc")).toEqual(["field_a", "field_b", "sum"]);
    const use = at("body.inc", "return f|ield_a");
    expect(s.show(await s.hover(use))).toContain("field `field_a`");
    expect(await s.definition(use)).toEqual([
        {
            uri: s.uri("body.inc"),
            range: { start: { line: 0, character: 4 }, end: { line: 0, character: 11 } },
        },
    ]);
});

serve.files(
    {
        "errors.def": "ERROR(NotFound, 404)\nERROR(Forbidden, 403)\n",
        "codes.cpp":
            "enum class Code {\n#define ERROR(name, value) name = value,\n" +
            '#include "errors.def"\n#undef ERROR\n};\n' +
            "const char* text(Code c) {\n  switch (c) {\n" +
            "#define ERROR(name, value) case Code::name: return #name;\n" +
            '#include "errors.def"\n#undef ERROR\n  }\n  return "";\n}\n',
    },
    cxx17(["codes.cpp"]),
)("enumerator list fragment", async ({ s }) => {
    await s.clean("errors.def");
    expect(await names(s, "errors.def")).toEqual(["NotFound", "Forbidden"]);
    expect(s.show(await s.hover(at("errors.def", "No|tFound")))).toContain("enumerator `NotFound`");
});

serve.files(
    {
        "body.tcc": "public:\nint x;\n",
        "s.cpp": 'struct S {\n#include "body.tcc"\n};\nint main() { return S{}.x; }\n',
    },
    cxx17(["s.cpp"]),
)("tcc fragment", async ({ s }) => {
    await s.clean("body.tcc");
});

// The fragment leaves its struct open: the includer's brace closes it, and
// the includer's namespace stays open to the end of the unit.
serve.files(
    {
        "x.inc": "struct S {\nint a;\n",
        "lib.cpp": 'namespace lib {\n#include "x.inc"\n}\nint main() { return 0; }\n',
    },
    cxx17(["lib.cpp"]),
)("unclosed host scope", async ({ s }) => {
    await s.compiled("x.inc");
    expect(await names(s, "x.inc")).toEqual(["S"]);
});

// The header's enum enters the list first; the source's table enters it
// again after including the header.
serve.files(
    {
        "ops.def": "OP(Add)\nOP(Sub)\n",
        "ops.h": '#pragma once\nenum Op {\n#define OP(x) x,\n#include "ops.def"\n#undef OP\n};\n',
        "ops.cpp":
            '#include "ops.h"\nconst char* names[] = {\n#define OP(x) #x,\n#include "ops.def"\n' +
            "#undef OP\n};\nint main() { return Add; }\n",
    },
    cxx17(["ops.cpp"]),
)("def entered twice", async ({ s }) => {
    await s.clean("ops.def");
    expect(await names(s, "ops.def")).toEqual(["Add", "Sub"]);

    const switched = await s.client.switchContext(s.uri("ops.def"), s.uri("ops.cpp"), {
        occurrence: 1,
    });
    expect(switched.success).toBe(true);
    await s.clean("ops.def");
});

// The macro the includer defines before the include point resolves to its
// definition there.
serve.files(
    {
        "list.def": "ITEM(alpha)\n",
        "main.cpp":
            '#define ITEM(name) int name;\n#include "list.def"\n#undef ITEM\nint main() { return alpha; }\n',
    },
    cxx17(["main.cpp"]),
)("host macro definition", async ({ s }) => {
    s.open("main.cpp");
    await s.indexed();
    expect(await indexes(s, "main")).toBe(true);
    await s.compiled("list.def");
    expect(await s.definition(at("list.def", "I|TEM"))).toEqual([
        {
            uri: s.uri("main.cpp"),
            range: { start: { line: 0, character: 8 }, end: { line: 0, character: 12 } },
        },
    ]);
});
