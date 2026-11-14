import { test } from "@clice/tools/testing";

test("missing forced header", async (t) => {
    // The command names a header that is not there: the error has no line
    // of the file to stand on and lands at its start.
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
