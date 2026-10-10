/// clice/counterparts answers from what the build and the index know, with
/// no compile of its own: files of the same name before the index arrives,
/// the declarations a source defines once it has.

import type { Serve } from "@clice/tools/actions";
import type { CounterpartsResult } from "@clice/tools/protocol";
import { expect, serve } from "../../fixtures.ts";

const test = serve("shapes/headers");

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

serve("shapes/headers", { config: { project: { enable_indexing: false } } })(
    "same name before the index",
    async ({ s }) => {
        expect(await shown(s, s.file("circle"))).toEqual({
            preferred: "src/circle.cpp",
            candidates: [["src/circle.cpp", "same name"]],
        });
        expect(await shown(s, s.file("circle_impl"))).toEqual({
            preferred: "include/shapes/circle.h",
            candidates: [["include/shapes/circle.h", "same name"]],
        });
    },
);

test("definitions rank the sources", async ({ s }) => {
    await s.indexed();
    expect(await shown(s, s.file("circle"))).toEqual({
        preferred: "src/circle.cpp",
        candidates: [
            ["src/circle.cpp", "defines 4 of 6 declarations", "same name"],
            ["src/measure.cpp", "defines 2 of 6 declarations"],
        ],
    });
    expect(await shown(s, s.file("measure"))).toEqual({
        preferred: "include/shapes/circle.h",
        candidates: [["include/shapes/circle.h", "declares 2 of 2 definitions"]],
    });
});

test("an edited buffer defines the declarations", async ({ s }) => {
    await s.indexed();
    const measure = s.file("measure");
    await s.compiled(measure);
    s.edit(s.file("measure"), {
        before: "}  // namespace shapes",
        insert:
            "double Circle::measure() const {\n    return 0;\n}\n\n" +
            'const char* Circle::name() const {\n    return "round";\n}\n\n' +
            "double area(const Circle& circle) {\n    return circle.radius;\n}\n\n",
    });
    await s.recompiled(measure);
    // The unsaved buffer comes before the disk: circle.cpp, still defining
    // those there, keeps only make_circle.
    expect(await shown(s, s.file("circle"))).toEqual({
        preferred: null,
        candidates: [
            ["src/circle.cpp", "defines 1 of 6 declarations", "same name"],
            ["src/measure.cpp", "defines 5 of 6 declarations"],
        ],
    });
});

test("nothing to pair with", async ({ s }) => {
    await s.indexed();
    expect(await counterparts(s, s.file("detail"))).toEqual({
        candidates: [],
        preferred: null,
    });
    expect(await counterparts(s, "include/shapes/missing.h")).toEqual({
        candidates: [],
        preferred: null,
    });
});
