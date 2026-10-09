/// clice/counterparts answers from what the build and the index know, with
/// no compile of its own: files of the same name before the index arrives,
/// the declarations a source defines once it has.

import { waitUntil } from "@clice/tools/client";
import type { CounterpartsResult } from "@clice/tools/protocol";
import { canonicalUri, type Workspace } from "@clice/tools/workspace";
import { expect, test } from "../fixtures.ts";

function writeProject(ws: Workspace): void {
    ws.write("include/codec.h", "#pragma once\nint encode(int value);\nint decode(int value);\n");
    ws.write(
        "src/codec.cpp",
        '#include "../include/codec.h"\nint encode(int value) { return value + 1; }\n',
    );
    ws.write(
        "src/decode.cpp",
        '#include "../include/codec.h"\nint decode(int value) { return value - 1; }\n',
    );
    ws.write("src/inline.h", "#pragma once\ninline int twice(int value) { return 2 * value; }\n");
    ws.write("src/main.cpp", '#include "inline.h"\nint main() { return twice(1); }\n');
    ws.writeCDB(["src/codec.cpp", "src/decode.cpp", "src/main.cpp"]);
}

/// The reply with every file named relative to the workspace.
function shown(ws: Workspace, result: CounterpartsResult) {
    const relative = (uri: string) => canonicalUri(uri).slice(canonicalUri(ws.uri()).length + 1);
    return {
        preferred: result.preferred === null ? null : relative(result.preferred),
        candidates: result.candidates.map((candidate) => [
            relative(candidate.uri),
            ...candidate.reasons,
        ]),
    };
}

test("same name before the index", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    const client = session.spawn(ws);
    await client.initialize(ws, { initializationOptions: { project: { enable_indexing: false } } });

    expect(shown(ws, await client.counterparts(ws.uri("include/codec.h")))).toEqual({
        preferred: "src/codec.cpp",
        candidates: [["src/codec.cpp", "same name"]],
    });
    expect(shown(ws, await client.counterparts(ws.uri("src/codec.cpp")))).toEqual({
        preferred: "include/codec.h",
        candidates: [["include/codec.h", "same name"]],
    });
});

test("definitions rank the sources", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    const client = session.spawn(ws);
    await client.initialize(ws);

    const header = ws.uri("include/codec.h");
    await waitUntil(async () => (await client.stats()).indexIdle, {
        timeout: 60_000,
        interval: 200,
        description: "the background index to catch up",
    });
    expect(shown(ws, await client.counterparts(header))).toEqual({
        preferred: "src/codec.cpp",
        candidates: [
            ["src/codec.cpp", "defines 1 of 2 declarations", "same name"],
            ["src/decode.cpp", "defines 1 of 2 declarations"],
        ],
    });
    expect(shown(ws, await client.counterparts(ws.uri("src/decode.cpp")))).toEqual({
        preferred: "include/codec.h",
        candidates: [["include/codec.h", "declares 1 of 1 definition"]],
    });
});

test("nothing to pair with", async ({ session }) => {
    const ws = session.tmpdir();
    writeProject(ws);
    const client = session.spawn(ws);
    await client.initialize(ws);

    await waitUntil(async () => (await client.stats()).indexIdle, {
        timeout: 60_000,
        interval: 200,
        description: "the background index to catch up",
    });
    expect(await client.counterparts(ws.uri("src/inline.h"))).toEqual({
        candidates: [],
        preferred: null,
    });
    expect(await client.counterparts(ws.uri("src/missing.h"))).toEqual({
        candidates: [],
        preferred: null,
    });
});
