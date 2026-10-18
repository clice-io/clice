import { test } from "@clice/tools/testing";

// a.h, b.h and c.cpp each nest a namespace N in an anonymous namespace:
// three namespaces of one hash, in units that share no file. References
// from one stay among the files of its own unit.
test("closed sources", async (t) => {
    await t.cli("index");
    await t.cli("query references a.cpp:2:22 --include-declaration");
    await t.cli("query references c.cpp:6:22 --include-declaration");
});
