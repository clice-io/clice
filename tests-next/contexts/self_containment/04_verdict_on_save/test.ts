import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The verdict was made on the buffer, which a save does not change: the
// header that gains its own include keeps the includer's context rather than
// compiling twice for the save.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("utils.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(1);
    s.edit("utils.h", { before: "inline int get_x", insert: '#include "types.h"\n' });
    expect(await s.errors("utils.h")).toEqual([]);
    s.save("utils.h");
    s.edit("utils.h", { after: "return p.x; }\n", insert: "\n" });
    await t.show("utils.h", s.diagnostics("utils.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
