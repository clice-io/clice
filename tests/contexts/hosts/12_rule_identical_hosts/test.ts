import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Two hosts whose commands differ only in what a rule removes compile the
// header the same way: one context stands for them.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("widget.cpp")).toEqual([]);
    expect(await s.errors("widget.h")).toEqual([]);
    await t.show("widget.h", s.contexts("widget.h"));
});
