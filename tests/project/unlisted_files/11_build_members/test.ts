import { test } from "@clice/tools/testing";

test("query lists the members", async (t) => {
    const s = await t.start();
    for (const [file, text] of [
        ["src/new.cpp", "int fresh_fn() { return 1; }\n"],
        ["src/orphan.h", "#pragma once\n"],
        ["out/gen.cpp", "int gen() { return 0; }\n"],
    ] as const) {
        t.disk.write(file, text);
        await s.diagnostics(file);
        s.save(file);
    }
    t.disk.write("src/opened.cpp", "int opened() { return 0; }\n");
    await s.diagnostics("src/opened.cpp");
    await s.stop();

    // Opening a source records it as saving does. A header is never a
    // member, and a build tree holds none.
    await t.cli("query project-files");
});
