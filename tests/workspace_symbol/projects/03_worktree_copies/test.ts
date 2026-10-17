import { test } from "@clice/tools/testing";
import { expect } from "vitest";

test(async (t) => {
    const s = await t.start({ folders: ["copy", "main", "other"] });
    expect(await s.errors("main/widget.cpp")).toEqual([]);
    await s.indexed();
    await t.show("widget_area", s.workspaceSymbols("widget_area"));
});
