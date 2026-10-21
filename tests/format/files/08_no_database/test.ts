import { test } from "@clice/tools/testing";

// A workspace with no compilation database, or an empty one: the run
// fails, saying so, unless files are named.
test("no database", async (t) => {
    await t.cli("format");
    await t.cli("format main.cpp");
});

test("an empty database", async (t) => {
    t.disk.write("compile_commands.json", "[]");
    await t.cli("format");
});
