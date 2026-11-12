import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A syntax error is the header's own, not a sign it needs its includer: a
// `;` missing before a `}`, the end or a keyword raises the error a macro of
// the includer's after a declarator does.
test(async (t) => {
    const s = await t.start();
    for (const header of ["typo.h", "semicolon.h", "last.h", "keyword.h"]) {
        await t.show(header, s.diagnostics(header));
    }
    expect((await s.stats()).synthesizedContexts).toBe(0);
});

// An edit while the header compiles in its host's context ends the
// comparison before it decides: the next compile compares again, and the
// tie keeps the header alone.
test("an edit cuts the comparison short", async (t) => {
    const s = await t.start();
    const alone = await s.park("compile", "typo.h", { at: "worker" });
    const inHost = await s.park("compile", "typo.h", { at: "worker" });
    s.open("typo.h", { pull: false });
    await alone.reached();
    await alone.release();
    await inHost.reached();
    s.edit("typo.h", { replace: "return }", with: "return  }" });
    await inHost.release();
    await t.show("edited", s.diagnostics("typo.h"));
    expect((await s.stats()).synthesizedContexts).toBe(0);
});
