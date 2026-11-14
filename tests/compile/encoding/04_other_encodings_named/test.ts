import { test } from "@clice/tools/testing";

// main.cpp is Latin-1: loading the project names the files that are no
// UTF-8, once.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await t.show("guidance", await s.guidance());
});
