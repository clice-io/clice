import { test } from "@clice/tools/testing";

// No entry of the database builds the standard library's module: the error
// says where its build command comes from.
test(async (t) => {
    const s = await t.start();
    await t.show("errors", s.errors("main.cpp"));
});
