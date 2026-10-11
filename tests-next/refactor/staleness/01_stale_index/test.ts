import { test, type TestContext } from "@clice/tools/testing";

/// `file:line:column` of the cursor `|` in the one place `anchor` occurs.
function place(t: TestContext, file: string, anchor: string): string {
    const text = t.disk.read(file);
    const snippet = anchor.replace("|", "");
    const begin = text.indexOf(snippet);
    if (begin < 0 || text.includes(snippet, begin + 1)) {
        throw new Error(`${file} does not hold ${JSON.stringify(snippet)} exactly once`);
    }
    const lines = text.slice(0, begin + anchor.indexOf("|")).split("\n");
    return `${file}:${String(lines.length)}:${String((lines.at(-1) ?? "").length + 1)}`;
}

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
    await t.cli(`refactor rename --symbol ${place(t, "new.cpp", "int |fresh")} --to evaluate`);
});
