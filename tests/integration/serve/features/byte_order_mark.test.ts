/// Files saved as "UTF-8 with BOM": every part of the server sees their text
/// without the mark, as an editor shows and sends it.

import * as proto from "vscode-languageserver-protocol";
import { at, expect, serve } from "../../fixtures.ts";

const BOM = "\uFEFF";

function startOf(location: proto.Location | proto.LocationLink | undefined): string | null {
    if (location === undefined) {
        return null;
    }
    const range = "targetUri" in location ? location.targetSelectionRange : location.range;
    return `${range.start.line}:${range.start.character}`;
}

serve.files({
    "a.h": `${BOM}int shared_fn();\n`,
    "c.cpp": `${BOM}int user3() { return 3; }\n#include "a.h"\n`,
    "main.cpp": `#include "a.h"\nint use() { return shared_fn(); }\n`,
})("first line positions skip the mark", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();

    const symbol = ((await s.workspaceSymbols("user3")) ?? []).find(
        (item) => item.name === "user3",
    );
    expect(symbol && "range" in symbol.location ? startOf(symbol.location) : null).toBe("0:4");
    const definition = await s.definition(at("main.cpp", "return s|hared_fn"));
    expect(startOf(Array.isArray(definition) ? definition[0] : (definition ?? undefined))).toBe(
        "0:4",
    );
});

serve.files({
    "types.h": "#pragma once\nstruct Point { int x; int y; };\n",
    "utils.h": "inline int get_x(Point p) { return p.x; }\n",
    "main.cpp": `${BOM}#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n`,
})("a marked host keeps the header context", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.clean("utils.h");
});

serve.files(
    {
        // The bytes EF BB BF 41 42.
        "data.bin": `${BOM}AB`,
        "main.cpp":
            'constexpr unsigned char data[] = {\n#embed "data.bin"\n};\nstatic_assert(sizeof(data) == 5);\n',
    },
    { manifest: { cxx: ["-std=c++26"], units: { "main.cpp": [] } } },
)("embedded data keeps the mark", async ({ s }) => {
    expect(await s.errors("main.cpp"), "`#embed` reads the file's bytes").toEqual([]);
});
