import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A switch recompiles the same version of the header: with no edit to make the
// editor ask again, the server tells it to refresh the tokens, and the tokens
// it then reads dim the other branch.
test(async (t) => {
    const s = await t.start({
        capabilities: { workspace: { semanticTokens: { refreshSupport: true } } },
    });
    s.open("render.h");
    await s.switchContext("render.h", "render_vk.cpp");
    expect(await s.errors("render.h")).toEqual([]);
    await t.show("vulkan host", s.inactiveLines("render.h"));
    const refreshes = await s.serverRequests("workspace/semanticTokens/refresh");
    await s.switchContext("render.h", "render_mt.cpp");
    expect(await s.errors("render.h")).toEqual([]);
    expect(await s.serverRequests("workspace/semanticTokens/refresh")).toBe(refreshes + 1);
    await t.show("metal host", s.inactiveLines("render.h"));
});
