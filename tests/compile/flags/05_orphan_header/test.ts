import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("orphan header compiles alone", async (t) => {
    // No database entry and no source including it: the header compiles
    // under the default command, as C++, in no host's context.
    const s = await t.start();
    expect(await s.errors("orphan.h")).toEqual([]);
    await t.show("contexts", s.contexts("orphan.h"));
    await t.show("hover", s.hover(t.at("orphan.h", "int |value")));
});
