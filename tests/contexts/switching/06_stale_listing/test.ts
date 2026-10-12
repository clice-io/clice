import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A save of new bytes moves the project on: a switch made from the listing
// before it is refused as stale, one from a fresh listing goes through.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("shared.h");
    const before = (await s.contexts("shared.h")).epoch;
    s.edit("main.cpp", { replace: "return 0", with: "return 1" });
    s.save("main.cpp");
    await s.sync();
    expect(await s.switchContext("shared.h", "main.cpp", { epoch: before })).toEqual({
        success: false,
        stale: true,
    });
    const fresh = (await s.contexts("shared.h")).epoch;
    expect(fresh).not.toBe(before);
    expect((await s.switchContext("shared.h", "main.cpp", { epoch: fresh })).success).toBe(true);
    await t.show("current", s.currentContext("shared.h"));
});
