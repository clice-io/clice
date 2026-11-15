import { test } from "@clice/tools/testing";

// No background index: the names come from the compile of the open file
// alone, under its PCH.
test(async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    s.open("main.cpp");
    for (const name of ["skipped", "defined"]) {
        await t.show(name, s.definition(t.marker(name)));
    }
});
