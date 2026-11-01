import { generate, test } from "@clice/tools/testing";

// More documents open than the pool keeps compiled: the first, evicted, is
// compiled again when asked instead of answering from a lost AST.

const UNITS = generate.units(18);

test(async (t) => {
    t.disk.write(UNITS);
    const s = await t.start({ config: { project: { max_resident_documents: 8 } } });
    const files = Object.keys(UNITS);
    for (const file of files) {
        await s.diagnostics(file);
    }
    await t.show("hover", s.hover(t.at(files[0] ?? "", "int |unit_")));
});
