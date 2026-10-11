import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The save that adds the include rescans what main.cpp includes: the new
// host is listed at once.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("lonely.h");
    await t.show("no includer", s.contexts("lonely.h"));
    s.edit("main.cpp", { text: '#include "lonely.h"\nint main() { return lonely(); }\n' });
    s.save("main.cpp");
    await t.show("included", s.contexts("lonely.h"));
});
