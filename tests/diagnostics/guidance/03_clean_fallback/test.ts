import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    for (const file of ["main.cpp", "typo.cpp"]) {
        await t.show(file, s.diagnostics(file));
    }
});
