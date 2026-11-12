import { test } from "@clice/tools/testing";

// p1 and p2 each declare a module named util. The implementation unit in
// p1 imports the nearest interface without writing the import: the warning
// naming the one passed over sits at the file's top.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("p1/util.cpp", s.diagnostics("p1/util.cpp"));
});
