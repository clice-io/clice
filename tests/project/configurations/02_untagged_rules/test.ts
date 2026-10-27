import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("untagged rules have no menu", async (t) => {
    const s = await t.start();
    await t.show("menu", s.configurations());
    expect(await s.switchConfiguration("debug")).toEqual({
        success: false,
        reason: "no rule declares the configuration debug",
    });
    // A selection another rule set left behind is ignored, not applied.
    await s.offline(() => {
        t.disk.write(".clice/state.json", '{"configuration": "debug"}\n');
    });
    await t.show("left behind", s.configurations());
});
