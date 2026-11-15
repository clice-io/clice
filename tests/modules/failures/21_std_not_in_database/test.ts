import { test } from "@clice/tools/testing";

// No entry of the database builds the standard library's module: the error
// says where its build command comes from.
test("database without std", async (t) => {
    t.disk.database({ "main.cpp": [] });
    const s = await t.start();
    await t.show("errors", s.errors("main.cpp"));
});

// With no database, the guessed command is part of the explanation.
test("no database", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});
