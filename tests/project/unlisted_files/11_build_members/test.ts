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

    // The command's arguments are the resolved compiler's, which differ on
    // every host; "provisional" against "inferred" in them is whether the
    // build counts the file a member, which this lists. A header is never
    // one, a build tree holds none, and opening records nothing.
    await t.cli("query --method projectFiles");
});
