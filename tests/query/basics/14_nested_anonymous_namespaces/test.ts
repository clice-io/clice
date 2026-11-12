import { test } from "@clice/tools/testing";

// a.h, b.h and c.cpp each nest a namespace N in an anonymous namespace:
// three namespaces, in units that share no file. References from one stay
// among the files of its own unit.
test("closed sources", async (t) => {
    await t.cli("index");
    await t.cli(`query references ${t.place(t.at("a.cpp", "return |N::x"))} --include-declaration`);
    await t.cli(`query references ${t.place(t.at("c.cpp", "return |N::z"))} --include-declaration`);
});
