import { test } from "@clice/tools/testing";

// A database that is no valid JSON is not loaded, and the message says where
// it breaks, not that an entry lacks its file.
test("command", { errors: ["cannot be loaded"] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});

test("editor", async (t) => {
    const s = await t.start();
    await s.diagnostics("a.cpp");
    await t.show("guidance", await s.guidance());
});
