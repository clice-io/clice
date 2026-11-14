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

// One a generator stopped writing ends early.
test("truncated", async (t) => {
    t.disk.write(
        "compile_commands.json",
        '[\n  {"file": "a.cpp", "arguments": ["clang++", "a.cpp"]',
    );
    const s = await t.start();
    await s.diagnostics("a.cpp");
    await t.show("guidance", await s.guidance());
});
