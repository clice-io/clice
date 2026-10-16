import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {}, publishDiagnostics: { relatedInformation: true } },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// Switching the header's context compiles it again with no edit of its own:
// the report the client pulled went stale, and it is asked to pull again.
test(async (t) => {
    const s = await t.start({ capabilities: PULL });
    expect(await s.errors("int_host.cpp")).toEqual([]);
    await s.diagnostics("bad_host.cpp");
    s.open("shared.h");
    expect((await s.switchContext("shared.h", "int_host.cpp")).success).toBe(true);
    await t.show("int host", s.diagnostics("shared.h"));
    const refreshes = await s.serverRequests(REFRESH);
    expect((await s.switchContext("shared.h", "bad_host.cpp")).success).toBe(true);
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await t.show("bad host", s.diagnostics("shared.h"));
});
