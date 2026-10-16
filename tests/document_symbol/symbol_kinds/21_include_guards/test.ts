import { answers, test } from "@clice/tools/testing";

// The headers, each opened in the context of the source that includes it;
// the markers only make the server open them. widget.h's guard is how it
// keeps itself from being read twice, nothing it declares; unguarded.h
// defines its macro under a condition that leaves code outside it, and
// limits.h gives a macro a default unless the includer set it: neither
// guards anything.
answers("headers", async (a) => {
    for (const file of ["widget.h", "defined.h", "unguarded.h", "limits.h"]) {
        await a.show(file, a.documentSymbols(file), { parts: ["detail"] });
    }
});

// needs_host.h compiles only after what its includer declares before it,
// which clice inspect, compiling it alone, cannot see.
test("in its includer's context", async (t) => {
    const s = await t.start();
    s.open("needs_host.h");
    await s.sync();
    await t.show("needs_host.h", s.documentSymbols("needs_host.h"));
});
