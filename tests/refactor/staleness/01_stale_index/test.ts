import { test } from "@clice/tools/testing";

test("a changed file", async (t) => {
    await t.cli("index --workers 2");
    t.disk.edit("other.cpp", {
        after: "compute(3); }\n",
        insert: "int more() { return compute(4); }\n",
    });
    await t.cli("refactor rename --symbol compute --to evaluate");
    await t.cli("refactor rename --symbol compute --to evaluate --fresh --dry-run");
});

test("a unit never indexed", async (t) => {
    await t.cli("index --workers 2");
    t.disk.write("late.cpp", '#include "lib.h"\n\nint later() { return compute(5); }\n');
    t.disk.database();
    await t.cli("refactor rename --symbol compute --to evaluate --dry-run");
    await t.cli("refactor rename --symbol compute --to evaluate --fresh --dry-run");
});

test("a file outside the build", async (t) => {
    await t.cli("index --workers 2");
    t.disk.write("new.cpp", "int fresh() { return 1; }\n");
    await t.cli(`refactor rename --symbol ${t.place(t.at("new.cpp", "int |fresh"))} --to evaluate`);
});
