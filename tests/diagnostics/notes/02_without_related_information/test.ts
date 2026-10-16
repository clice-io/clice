import { test } from "@clice/tools/testing";

// A client that takes no related information gets the notes in the message.
test(async (t) => {
    const s = await t.start({ capabilities: {} });
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
