import type { Serve, TestContext } from "@clice/tools/testing";
import { test } from "@clice/tools/testing";

// Saving a header reindexes the closed sources including it: the macro
// retargets closed.cpp's call from alpha to beta, which only a reindex
// against the saved header can see.
async function show(t: TestContext, s: Serve, label: string) {
    await t.show(label, {
        alpha: s.references(t.at("header.h", "int |alpha()")),
        beta: s.references(t.at("header.h", "int |beta()")),
    });
}

test("header save reindexes dependents", async (t) => {
    const s = await t.start();
    s.open("header.h");
    await s.indexed();
    await show(t, s, "before");
    s.edit("header.h", { replace: "#define TARGET alpha", with: "#define TARGET beta" });
    s.save("header.h");
    await s.sync();
    await show(t, s, "after");
});

// A save hook rewrote the file as the save landed: the disk holds the new
// text while the buffer still holds the old, and no change is ever sent.
test("divergent save follows disk", async (t) => {
    const s = await t.start();
    s.open("header.h");
    await s.indexed();
    t.disk.edit("header.h", { replace: "#define TARGET alpha", with: "#define TARGET beta" });
    s.save("header.h", { write: false });
    await s.sync();
    await show(t, s, "after");
});
