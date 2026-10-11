import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    for (const file of ["missing.cpp", "forced.cpp"]) {
        await t.show(file, s.diagnostics(file), { parts: ["related"] });
    }
});
