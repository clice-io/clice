import { test } from "@clice/tools/testing";

// Only a check about something deprecated strikes the code through; one
// suggesting a modern spelling leaves it as it is.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
});
