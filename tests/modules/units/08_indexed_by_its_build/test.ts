import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// math.cppm compiles once, into its PCM: the build indexes the unit too,
// and the background index finds that index current.
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

// An importer completes the names the module exports from the PCM its
// compile loads, an enumerator `using enum` brings into the namespace among
// them; a name of a header the global module fragment includes comes from
// the index the PCM build merged, with that header's #include, and the
// unit's own not at all.
test("completion", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("math.cppm")).toEqual([]);
    s.open("main.cpp");
    s.edit("main.cpp", {
        text:
            "import math;\nint total = math::tw;\nint other = hid;\nint more = clam;\n" +
            "auto tone = math::tone_w;\n",
    });
    await t.show("qualified", s.completion(t.at("main.cpp", "math::tw|;")));
    await t.show("brought_in", s.completion(t.at("main.cpp", "math::tone_w|;")));
    await t.show("unexported", s.completion(t.at("main.cpp", "= hid|;")));
    await t.show("fragment", s.completion(t.at("main.cpp", "= clam|;")), { parts: ["edits"] });
});
