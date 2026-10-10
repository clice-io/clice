/// clice/counterparts answers from what the build and the index know, with
/// no compile of its own: files of the same name before the index arrives,
/// the declarations a source defines once it has.

import type { Serve } from "@clice/tools/actions";
import type { CounterpartsResult } from "@clice/tools/protocol";
import { expect, serve } from "../../fixtures.ts";

const FILES = {
    "include/codec.h": "#pragma once\nint encode(int value);\nint decode(int value);\n",
    "src/codec.cpp": '#include "../include/codec.h"\nint encode(int value) { return value + 1; }\n',
    "src/decode.cpp":
        '#include "../include/codec.h"\nint decode(int value) { return value - 1; }\n',
    "src/inline.h": "#pragma once\ninline int twice(int value) { return 2 * value; }\n",
    "src/main.cpp": '#include "inline.h"\nint main() { return twice(1); }\n',
};

const test = serve.files(FILES);

function counterparts(s: Serve, file: string): Promise<CounterpartsResult> {
    return s.request<CounterpartsResult>("clice/counterparts", file, { uri: s.uri(file) });
}

/// The reply with every file named relative to the workspace.
async function shown(s: Serve, file: string) {
    const result = await counterparts(s, file);
    return {
        preferred: result.preferred === null ? null : s.relative(result.preferred),
        candidates: result.candidates.map((candidate) => [
            s.relative(candidate.uri),
            ...candidate.reasons,
        ]),
    };
}

serve.files(FILES, { config: { project: { enable_indexing: false } } })(
    "same name before the index",
    async ({ s }) => {
        expect(await shown(s, "include/codec.h")).toEqual({
            preferred: "src/codec.cpp",
            candidates: [["src/codec.cpp", "same name"]],
        });
        expect(await shown(s, "src/codec.cpp")).toEqual({
            preferred: "include/codec.h",
            candidates: [["include/codec.h", "same name"]],
        });
    },
);

test("definitions rank the sources", async ({ s }) => {
    await s.indexed();
    expect(await shown(s, "include/codec.h")).toEqual({
        preferred: "src/codec.cpp",
        candidates: [
            ["src/codec.cpp", "defines 1 of 2 declarations", "same name"],
            ["src/decode.cpp", "defines 1 of 2 declarations"],
        ],
    });
    expect(await shown(s, "src/decode.cpp")).toEqual({
        preferred: "include/codec.h",
        candidates: [["include/codec.h", "declares 1 of 1 definition"]],
    });
});

test("an edited buffer defines the declarations", async ({ s }) => {
    await s.indexed();
    await s.compiled("src/decode.cpp");
    s.edit("src/decode.cpp", {
        replace: "int decode(int value) { return value - 1; }",
        with: "int encode(int value) { return value + 2; }\nint decode(int value) { return value - 2; }",
    });
    await s.recompiled("src/decode.cpp");
    // The unsaved buffer comes before the disk: codec.cpp, still defining
    // encode there, pairs by its name alone.
    expect(await shown(s, "include/codec.h")).toEqual({
        preferred: null,
        candidates: [
            ["src/decode.cpp", "defines 2 of 2 declarations"],
            ["src/codec.cpp", "same name"],
        ],
    });
});

test("nothing to pair with", async ({ s }) => {
    await s.indexed();
    expect(await counterparts(s, "src/inline.h")).toEqual({
        candidates: [],
        preferred: null,
    });
    expect(await counterparts(s, "src/missing.h")).toEqual({
        candidates: [],
        preferred: null,
    });
});
