import { test } from "@clice/tools/testing";

// Where the command of each file comes from: its database entry, the
// source including the header, the unit beside it, a rule's
// default_command, and clice's own for a C file no C unit lends to.
test(async (t) => {
    const s = await t.start();
    await s.sync();
    for (const file of [
        "src/main.cpp",
        "src/shared.h",
        "src/extra.cpp",
        "tools/gen.cpp",
        "scratch/plain.c",
    ]) {
        s.open(file, { pull: false });
        await t.show(file, s.compileCommand(file));
    }
});
