import { test } from "@clice/tools/testing";

// A database that loads and lists no file is no missing one: the guidance
// says what it found.
test(async (t) => {
    t.disk.write("compile_commands.json", "[]");
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await t.show("guidance", await s.guidance());
});
