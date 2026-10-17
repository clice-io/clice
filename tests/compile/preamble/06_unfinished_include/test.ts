import { test } from "@clice/tools/testing";

// The user types a new include in the middle of the block and has not
// closed its name yet: only that line is wrong.
test("typed mid-block", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.errors("main.cpp");
    s.edit("main.cpp", { after: '#include "a.h"\n', insert: "#include <vec\n" });
    await t.show("errors", s.errors("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "a() + |c()")));
});

test("file cut inside the preamble", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.errors("main.cpp");
    s.edit("main.cpp", { text: '#include "a.h"\n#include "c' });
    await t.show("errors", s.errors("main.cpp"));
});
