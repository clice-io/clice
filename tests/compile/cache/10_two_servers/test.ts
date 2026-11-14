import { test } from "@clice/tools/testing";

// Two editor windows on one project: the second finds the index the first
// keeps and serves nothing there, while the first serves on.
test("a second window is refused", async (t) => {
    const first = await t.start();
    await first.diagnostics("main.cpp");
    t.disk.write("header.h", "#pragma once\n\nstruct Shared {\n    int y;\n};\n");
    await t.show("changed", first.errors("main.cpp"));
    const second = await t.start({ env: { CLICE_TEST_WRITER_WAIT_MS: "1000" } });
    // Asked while the second waits for the index, answered once refused.
    const refused = second.diagnostics("main.cpp");
    await t.show("guidance", await second.guidance());
    await t.show("refused", refused);
});
