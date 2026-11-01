import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The header compiles after what main.cpp declares first, which the index of
// its PCH, built behind that text, does not name: the completion reads it
// from the PCH whole.
test("names of the host", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("ruler.h")).toEqual([]);
    await t.show("type", s.completion(t.at("ruler.h", "return Met|ers{")), { parts: ["insert"] });
    await t.show("macro", s.completion(t.at("ruler.h", "{METERS_|SCALE")), { parts: ["insert"] });
});
