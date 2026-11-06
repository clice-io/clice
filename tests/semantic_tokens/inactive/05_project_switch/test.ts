import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A switch to a host of another project recompiles the same version of the
// header there: the server tells the editor to refresh the tokens, which
// then dim the other branch.
test(async (t) => {
    const s = await t.start({
        folders: ["app", "lib"],
        capabilities: { workspace: { semanticTokens: { refreshSupport: true } } },
    });
    s.open("lib/include/render.h");
    expect(await s.errors("lib/include/render.h")).toEqual([]);
    await t.show("library host", s.inactiveLines("lib/include/render.h"));
    const refreshes = await s.serverRequests("workspace/semanticTokens/refresh");
    expect((await s.switchContext("lib/include/render.h", "app/main.cpp")).success).toBe(true);
    expect(await s.errors("lib/include/render.h")).toEqual([]);
    expect(await s.serverRequests("workspace/semanticTokens/refresh")).toBe(refreshes + 1);
    await t.show("application host", s.inactiveLines("lib/include/render.h"));
});
