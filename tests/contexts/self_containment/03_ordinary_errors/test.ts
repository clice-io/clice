import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A syntax error is the header's own, not a sign it needs its includer: a
// `;` missing before a `}` or the end raises the error a macro of the
// includer's after a declarator does.
test(async (t) => {
    const s = await t.start();
    for (const header of ["typo.h", "semicolon.h", "last.h"]) {
        await t.show(header, s.diagnostics(header));
    }
    expect((await s.stats()).synthesizedContexts).toBe(0);
});
