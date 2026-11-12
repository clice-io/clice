import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

const IMPORTERS = ["main.cpp", "bar.cppm", "impl.cpp"];

answers(
    "compiled",
    { paths: { server: "clice inspect compiles one file alone, and an importer has no answer" } },
    async (a) => {
        for (const file of IMPORTERS) {
            await a.show(file, a.semanticTokens(file));
        }
    },
);

// The tokens the index serves an importer before it compiles.
test("index", async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    for (const file of IMPORTERS) {
        s.open(file, { pull: false });
    }
    await s.sync();
    for (const file of IMPORTERS) {
        await t.show(file, s.semanticTokens(file));
    }
    expect((await s.counts()).compile).toBe(0);
});
