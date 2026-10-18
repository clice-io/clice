import { test } from "@clice/tools/testing";

// The root's database links the build's, and the rules name it under both
// spellings: the lib/ rule's own order decides, the build's database first.
test(
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("compile_commands.json", "build/compile_commands.json");
        const s = await t.start();
        await t.show("lib/x.cpp", s.errors("lib/x.cpp"));
    },
);
