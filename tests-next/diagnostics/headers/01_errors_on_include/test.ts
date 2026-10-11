import { test } from "@clice/tools/testing";

// bad.h is read after code in body.cpp and among the leading directives of
// pre.cpp, where a precompiled preamble holds it: the error reaches the
// include line either way. Opened, bad.h shows all of its own.
test(async (t) => {
    const s = await t.start();
    for (const file of ["body.cpp", "pre.cpp", "bad.h"]) {
        await t.show(file, s.diagnostics(file), { parts: ["related"] });
    }
});
