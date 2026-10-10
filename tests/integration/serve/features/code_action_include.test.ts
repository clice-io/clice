/// Add include on the server path: the directive lands where it always
/// applies, only headers a user may include are offered, and the edited
/// file compiles. A fake standard library under `sys/` keeps the tests
/// independent of the host's.

import type { Serve } from "@clice/tools/actions";
import { actionsOf, applyTextEdits, editsFor } from "@clice/tools/client/edits";
import type * as proto from "vscode-languageserver-protocol";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

const VECTOR = "#pragma once\nnamespace std {\ntemplate <class T> struct vector {};\n}\n";

const SYS = ["-std=c++17", "-nostdinc", "-Isys"];

async function includeActions(s: Serve, loc: Loc): Promise<proto.CodeAction[]> {
    return actionsOf(await s.codeActions(loc)).filter((action) =>
        action.title.startsWith("Add #include"),
    );
}

/// The index answers `name` with a symbol of `file`.
async function expectIndexed(s: Serve, name: string, file: string): Promise<void> {
    await s.indexed();
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    expect(
        symbols.some((symbol) => "uri" in symbol.location && symbol.location.uri.endsWith(file)),
        `${name} indexed from ${file}`,
    ).toBe(true);
}

const MACRO_MAIN = "#ifndef MAX_SIZE\n#define MAX_SIZE 64\n#endif\n\nstd::vector<int> values;\n";

serve.files(
    { "sys/vector": VECTOR, "main.cpp": MACRO_MAIN },
    { manifest: { args: [...SYS, "-DMAX_SIZE=32"], units: { "main.cpp": [] } } },
)("feature macro block is no guard", async ({ s }) => {
    await s.compiled("main.cpp");
    const loc = at("main.cpp", "std::v|ector<int> values");
    const [action] = await includeActions(s, loc);
    expect(action?.title).toBe("Add #include <vector>");
    const { text, diagnostics } = await s.applyAction(loc, "Add #include <vector>");
    expect(text).toBe(`#include <vector>\n${MACRO_MAIN}`);
    expect(diagnostics).toEqual([]);
});

const EMBEDDED_MAIN =
    '#include "config.h"\n' +
    'extern "C" {\n#include "c_api.h"\n}\n' +
    'enum Color {\n#include "colors.def"\n};\n' +
    "std::vector<Color> palette;\n" +
    '#include "palette.tpp"\n';

serve.files(
    {
        "sys/vector": VECTOR,
        "config.h": "#pragma once\n",
        "c_api.h": "#pragma once\nint c_api(void);\n",
        "colors.def": "RED,\nGREEN,\n",
        "palette.tpp": "inline int palette_size() { return 2; }\n",
        "main.cpp": EMBEDDED_MAIN,
    },
    { manifest: { args: SYS, units: { "main.cpp": [] } } },
)("include skips embedded and trailing ones", async ({ s }) => {
    await s.compiled("main.cpp");
    const loc = at("main.cpp", "std::v|ector<Color>");
    const [action] = await includeActions(s, loc);
    expect(action?.title).toBe("Add #include <vector>");
    const { text, diagnostics } = await s.applyAction(loc, "Add #include <vector>");
    expect(text).toBe(EMBEDDED_MAIN.replace('"config.h"\n', '"config.h"\n#include <vector>\n'));
    expect(diagnostics).toEqual([]);
});

const INTERNAL_MAIN = "using namespace std;\nvector<int> a;\ndeque<int> b;\nstd::string c;\n";

serve.files(
    {
        "sys/vector": "#pragma once\n#include <bits/stl_vector.h>\n",
        "sys/bits/stl_vector.h": VECTOR,
        "sys/deque": "#pragma once\n#include <__deque/deque.h>\n",
        "sys/__deque/deque.h":
            "#pragma once\nnamespace std {\ntemplate <class T> struct deque {};\n}\n",
        "sys/string": "#pragma once\n#include <xstring>\n",
        "sys/xstring": "#pragma once\nnamespace std {\nstruct string {};\n}\n",
        "other.cpp":
            "#include <vector>\n#include <deque>\n#include <string>\n" +
            "std::vector<int> a;\nstd::deque<int> b;\nstd::string c;\n",
        "main.cpp": INTERNAL_MAIN,
    },
    {
        config: { project: { enable_indexing: true } },
        manifest: { args: SYS, units: { "main.cpp": [], "other.cpp": [] } },
    },
)("standard names skip internal headers", async ({ s }) => {
    await s.compiled("main.cpp");
    await expectIndexed(s, "vector", "/stl_vector.h");
    await expectIndexed(s, "deque", "/deque.h");
    await expectIndexed(s, "string", "/xstring");

    // libstdc++'s `bits/` and libc++'s `__` headers are filtered from the
    // index's answer; a qualified standard name asks no index at all,
    // which would offer the internal <xstring> too.
    const uri = s.uri("main.cpp");
    const edits: proto.TextEdit[] = [];
    for (const [loc, header] of [
        [at("main.cpp", "vector<int> a"), "<vector>"],
        [at("main.cpp", "deque<int> b"), "<deque>"],
        [at("main.cpp", "std::s|tring c"), "<string>"],
    ] as const) {
        const actions = await includeActions(s, loc);
        expect(actions.map((action) => action.title)).toEqual([`Add #include ${header}`]);
        const start = { line: 0, character: 0 };
        expect(editsFor(actions[0]!, uri)).toEqual([
            { range: { start, end: start }, newText: `#include ${header}\n` },
        ]);
        edits.push(...editsFor(actions[0]!, uri));
    }
    s.edit("main.cpp", { text: applyTextEdits(INTERNAL_MAIN, edits) });
    await s.clean("main.cpp");
});

serve.files(
    {
        "lib.h": "#pragma once\nint count(int);\n",
        "lib.cpp": '#include "lib.h"\nint count(int n) { return n; }\n',
        "main.cpp": "struct Box {};\nint use(Box* box) { return box->count; }\n",
    },
    {
        config: { project: { enable_indexing: true } },
        manifest: { cxx: ["-std=c++17"], units: { "main.cpp": [], "lib.cpp": [] } },
    },
)("member access offers no include", async ({ s }) => {
    await s.compiled("main.cpp");
    await expectIndexed(s, "count", "/lib.cpp");

    expect(await includeActions(s, at("main.cpp", "box->c|ount"))).toEqual([]);
});

serve.files(
    {
        "types.h": "#pragma once\nstruct Point { int x; int y; };\n",
        "lib.h": "#pragma once\nint helper();\n",
        "lib.cpp": '#include "lib.h"\nint helper() { return 1; }\n',
        "utils.h": "#pragma once\n\ninline int get_x(Point p) { return helper() + p.x; }\n",
        "main.cpp":
            '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    },
    {
        config: { project: { enable_indexing: true } },
        manifest: { cxx: ["-std=c++17"], units: { "main.cpp": [], "lib.cpp": [] } },
    },
)("context header keeps its directive inside", async ({ s }) => {
    await s.compiled("main.cpp");
    await expectIndexed(s, "helper", "/lib.cpp");
    await s.compiled("utils.h");
    expect((await s.stats()).synthesizedContexts).toBe(1);

    const loc = at("utils.h", "h|elper()");
    const [action] = await includeActions(s, loc);
    expect(action?.title).toBe('Add #include "lib.h"');
    const { text, diagnostics } = await s.applyAction(loc, 'Add #include "lib.h"');
    expect(text).toBe(
        '#pragma once\n#include "lib.h"\n\ninline int get_x(Point p) { return helper() + p.x; }\n',
    );
    expect(diagnostics).toEqual([]);
});
