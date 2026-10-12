import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("context from another folder", async (t) => {
    const s = await t.start({ folders: ["app", "lib"] });
    // The library's own source is the header's host at first; the
    // application's is on offer too.
    await t.show("library host", {
        errors: s.errors("lib/include/lib.h"),
        contexts: s.contexts("lib/include/lib.h"),
    });
    expect((await s.switchContext("lib/include/lib.h", "app/main.cpp")).success).toBe(true);
    expect(await s.errors("lib/include/lib.h")).toEqual([]);
    await t.show("switched", s.currentContext("lib/include/lib.h"));
    // The reset hands the header back to its own folder's host.
    expect((await s.resetContext("lib/include/lib.h")).success).toBe(true);
    await t.show("reset", {
        errors: s.errors("lib/include/lib.h"),
        current: s.currentContext("lib/include/lib.h"),
    });
    // The application's project retracts what it pushed.
    await t.show("pushes", s.pushes("lib/include/lib.h"));
});
