import { test } from "@clice/tools/testing";

// Headers opened alone borrow user.cpp's command and stay headers: the broken
// include keeps hdr.h out of a precompiled preamble, so the main parse sees
// its #pragma once and must not call it one in the main file; the
// system-header pragma and the unused static functions are for includers.
// mylib has no extension and is a header all the same.
test(async (t) => {
    const s = await t.start();
    for (const file of ["src/hdr.h", "src/util.h", "src/mylib"]) {
        await t.show(file, s.diagnostics(file));
    }
});
