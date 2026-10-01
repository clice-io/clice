/// Add include on the server path: the directive lands where it always
/// applies, only headers a user may include are offered, and the edited
/// file compiles. A fake standard library under `sys/` keeps the tests
/// independent of the host's.

import type * as proto from "vscode-languageserver-protocol";
import { SETTLE_TIME, waitUntil, type CliceClient } from "@clice/tools/client";
import { applyTextEdits, editsFor } from "@clice/tools/client/edits";
import { expect, test } from "../fixtures.ts";

const VECTOR = "#pragma once\nnamespace std {\ntemplate <class T> struct vector {};\n}\n";

function at(line: number, character: number): proto.Range {
    return { start: { line, character }, end: { line, character } };
}

async function includeActions(
    client: CliceClient,
    uri: string,
    range: proto.Range,
): Promise<proto.CodeAction[]> {
    const reply = (await client.codeActions(uri, range)) ?? [];
    return reply.filter(
        (item): item is proto.CodeAction =>
            "title" in item && item.title.startsWith("Add #include"),
    );
}

/// The buffer after `action`, recompiled: its diagnostics are the
/// edited file's.
async function apply(
    client: CliceClient,
    uri: string,
    text: string,
    action: proto.CodeAction,
): Promise<string> {
    const applied = applyTextEdits(text, editsFor(action, uri));
    client.change(uri, 1, applied);
    await client.waitForRecompile(uri);
    return applied;
}

async function waitIndexed(client: CliceClient, name: string, file: string): Promise<void> {
    await waitUntil(
        async () =>
            ((await client.workspaceSymbols(name)) ?? []).some((symbol) =>
                symbol.location.uri.endsWith(file),
            ),
        { timeout: 30_000, interval: SETTLE_TIME, description: `${name} indexed from ${file}` },
    );
}

test("feature macro block is no guard", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("sys/vector", VECTOR);
    workspace.write(
        "main.cpp",
        "#ifndef MAX_SIZE\n#define MAX_SIZE 64\n#endif\n\nstd::vector<int> values;\n",
    );
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-nostdinc", "-Isys", "-DMAX_SIZE=32"] });
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri, text] = await client.openAndWait("main.cpp");

    const [action] = await includeActions(client, uri, at(4, 6));
    expect(action?.title).toBe("Add #include <vector>");
    expect(await apply(client, uri, text, action!)).toBe(`#include <vector>\n${text}`);
    client.assertCleanCompile(uri);
});

test("include skips embedded and trailing ones", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("sys/vector", VECTOR);
    workspace.write("config.h", "#pragma once\n");
    workspace.write("c_api.h", "#pragma once\nint c_api(void);\n");
    workspace.write("colors.def", "RED,\nGREEN,\n");
    workspace.write("palette.tpp", "inline int palette_size() { return 2; }\n");
    const text =
        '#include "config.h"\n' +
        'extern "C" {\n#include "c_api.h"\n}\n' +
        'enum Color {\n#include "colors.def"\n};\n' +
        "std::vector<Color> palette;\n" +
        '#include "palette.tpp"\n';
    workspace.write("main.cpp", text);
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-nostdinc", "-Isys"] });
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const [action] = await includeActions(client, uri, at(7, 6));
    expect(action?.title).toBe("Add #include <vector>");
    expect(await apply(client, uri, text, action!)).toBe(
        text.replace('"config.h"\n', '"config.h"\n#include <vector>\n'),
    );
    client.assertCleanCompile(uri);
});

test("standard names skip internal headers", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("sys/vector", "#pragma once\n#include <bits/stl_vector.h>\n");
    workspace.write("sys/bits/stl_vector.h", VECTOR);
    workspace.write("sys/deque", "#pragma once\n#include <__deque/deque.h>\n");
    workspace.write(
        "sys/__deque/deque.h",
        "#pragma once\nnamespace std {\ntemplate <class T> struct deque {};\n}\n",
    );
    workspace.write("sys/string", "#pragma once\n#include <xstring>\n");
    workspace.write("sys/xstring", "#pragma once\nnamespace std {\nstruct string {};\n}\n");
    workspace.write(
        "other.cpp",
        "#include <vector>\n#include <deque>\n#include <string>\n" +
            "std::vector<int> a;\nstd::deque<int> b;\nstd::string c;\n",
    );
    const text = "using namespace std;\nvector<int> a;\ndeque<int> b;\nstd::string c;\n";
    workspace.write("main.cpp", text);
    workspace.writeCDB(["main.cpp", "other.cpp"], { extraArgs: ["-nostdinc", "-Isys"] });
    const client = await session
        .spawn(workspace)
        .initialize(workspace, { initializationOptions: { project: { enable_indexing: true } } });
    const [uri] = await client.openAndWait("main.cpp");
    await waitIndexed(client, "vector", "/stl_vector.h");
    await waitIndexed(client, "deque", "/deque.h");
    await waitIndexed(client, "string", "/xstring");

    // libstdc++'s `bits/` and libc++'s `__` headers are filtered from the
    // index's answer; a qualified standard name asks no index at all,
    // which would offer the internal <xstring> too.
    const edits: proto.TextEdit[] = [];
    for (const [range, header] of [
        [at(1, 0), "<vector>"],
        [at(2, 0), "<deque>"],
        [at(3, 6), "<string>"],
    ] as const) {
        const actions = await includeActions(client, uri, range);
        expect(actions.map((action) => action.title)).toEqual([`Add #include ${header}`]);
        expect(editsFor(actions[0]!, uri)).toEqual([
            { range: at(0, 0), newText: `#include ${header}\n` },
        ]);
        edits.push(...editsFor(actions[0]!, uri));
    }
    client.change(uri, 1, applyTextEdits(text, edits));
    await client.waitForRecompile(uri);
    client.assertCleanCompile(uri);
});

test("member access offers no include", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("lib.h", "#pragma once\nint count(int);\n");
    workspace.write("lib.cpp", '#include "lib.h"\nint count(int n) { return n; }\n');
    workspace.write("main.cpp", "struct Box {};\nint use(Box* box) { return box->count; }\n");
    workspace.writeCDB(["main.cpp", "lib.cpp"]);
    const client = await session
        .spawn(workspace)
        .initialize(workspace, { initializationOptions: { project: { enable_indexing: true } } });
    const [uri] = await client.openAndWait("main.cpp");
    await waitIndexed(client, "count", "/lib.cpp");

    expect(await includeActions(client, uri, at(1, 33))).toEqual([]);
});

test("context header keeps its directive inside", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("types.h", "#pragma once\nstruct Point { int x; int y; };\n");
    workspace.write("lib.h", "#pragma once\nint helper();\n");
    workspace.write("lib.cpp", '#include "lib.h"\nint helper() { return 1; }\n');
    const header = "#pragma once\n\ninline int get_x(Point p) { return helper() + p.x; }\n";
    workspace.write("utils.h", header);
    workspace.write(
        "main.cpp",
        '#include "types.h"\n#include "utils.h"\nint main() { return get_x({1, 2}); }\n',
    );
    workspace.writeCDB(["main.cpp", "lib.cpp"]);
    const client = await session
        .spawn(workspace)
        .initialize(workspace, { initializationOptions: { project: { enable_indexing: true } } });
    await client.openAndWait("main.cpp");
    await waitIndexed(client, "helper", "/lib.cpp");
    const [uri] = await client.openAndWait("utils.h");
    expect((await client.stats()).synthesizedContexts).toBe(1);

    const [action] = await includeActions(client, uri, at(2, 36));
    expect(action?.title).toBe('Add #include "lib.h"');
    expect(await apply(client, uri, header, action!)).toBe(
        '#pragma once\n#include "lib.h"\n\ninline int get_x(Point p) { return helper() + p.x; }\n',
    );
    client.assertCleanCompile(uri);
});
