import { test } from "@clice/tools/testing";

test(async (t) => {
    t.disk.write("main.cpp", t.disk.read("main.cpp").replaceAll("\n", "\r\n"));
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await t.show("main.cpp", s.semanticTokens("main.cpp"));
});
