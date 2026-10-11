import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Three hosts with the same flags read the header the same way: one context
// stands for them, the host whose name matches the header's. Folding them
// takes the header's own trial compile finding it self-contained.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("widget.cpp")).toEqual([]);
    expect(await s.errors("widget.h")).toEqual([]);
    await t.show("widget.h", s.contexts("widget.h"));
});
