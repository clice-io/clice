import { test } from "@clice/tools/testing";

// The .clang-tidy that does not parse was saved with a byte order mark:
// the notes lead to the places that do not parse in the text the editor
// shows.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
