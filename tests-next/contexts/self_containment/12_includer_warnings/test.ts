import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cpp's code after the include is main.cpp's: its deprecation warning
// does not move onto the header through the note that points there.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("api.h", s.diagnostics("api.h"));
    expect((await s.stats()).synthesizedContexts).toBe(1);
});
