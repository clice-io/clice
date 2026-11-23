import { test } from "@clice/tools/testing";

// clang's notes on a flag it refuses point at no place: they go into the
// error's message.
test(async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
