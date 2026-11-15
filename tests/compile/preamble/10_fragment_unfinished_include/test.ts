import { test } from "@clice/tools/testing";

// The user types a new include in the global module fragment, ahead of
// another include: only that line is wrong, and the module declaration stays
// the first declaration the compile parses.
test(async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    await s.errors("main.cppm");
    s.edit("main.cppm", { after: '#include "a.h"\n', insert: "#include <vec\n" });
    await t.show("errors", s.errors("main.cppm"));
});
