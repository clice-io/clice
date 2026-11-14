import { test } from "@clice/tools/testing";

// A file outside every project the editor opened compiles with a guessed
// command: the warning says it lies outside, and asks for no database.
test(async (t) => {
    const s = await t.start({ folders: ["app"] });
    await t.show("stray.cpp", s.diagnostics("loose/stray.cpp"));
});
