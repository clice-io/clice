import { test } from "@clice/tools/testing";

// The preamble's include, a comment before its file name, reaches the
// checks: the one after the preamble repeats it.
test(async (t) => {
    // Unused includes come with the background index, before this pull or
    // after it.
    const s = await t.start({ config: { diagnostics: { unused_includes: false } } });
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
