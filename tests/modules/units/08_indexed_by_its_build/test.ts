import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// math.cppm compiles once, into its PCM: the build indexes the unit too,
// and the background index takes that index for a compile of its own.
test("one compile", async (t) => {
    const s = await t.start();
    await s.indexed();
    const { files } = await s.counts();
    expect({ pcm: files["math.cppm"]?.pcm, index: files["math.cppm"]?.index ?? 0 }).toEqual({
        pcm: 1,
        index: 0,
    });
    await t.show("references", s.references(t.marker("add")));
    await t.show("header", s.references(t.at("limits.h", "inline int |clamp")));
});

// An importer completes the module's names from the index its PCM build
// wrote, as it does the preamble's: those it exports; a name of a header
// the global module fragment includes comes with that header's #include,
// and the unit's own not at all.
test("completion", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("math.cppm")).toEqual([]);
    s.open("main.cpp");
    s.edit("main.cpp", {
        text: "import math;\nint total = math::tw;\nint other = hid;\nint more = clam;\n",
    });
    await t.show("qualified", s.completion(t.at("main.cpp", "math::tw|;")));
    await t.show("unexported", s.completion(t.at("main.cpp", "= hid|;")));
    await t.show("fragment", s.completion(t.at("main.cpp", "= clam|;")), { parts: ["edits"] });
});
