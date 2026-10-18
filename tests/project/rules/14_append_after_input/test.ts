import { test } from "@clice/tools/testing";

// The database writes -std=c++17 after the file; the rule appends -std=c++20.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.errors("main.cpp"));
});
