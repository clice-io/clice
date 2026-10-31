import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A chosen host that includes the header through a macro, which only its
// include tree sees, still holds when another file changes: the include
// graph not reaching the header is no sign the host stopped including it.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: true } } });
    await s.indexed();
    s.open("include/widget.h");
    expect((await s.switchContext("include/widget.h", "main.cpp")).success).toBe(true);
    expect(await s.errors("include/widget.h")).toEqual([]);
    t.disk.edit("alt.cpp", { after: "int alt() {\n", insert: "    // changed\n" });
    await s.poll("workspace");
    await t.show("other host changed", s.currentContext("include/widget.h"));
});
