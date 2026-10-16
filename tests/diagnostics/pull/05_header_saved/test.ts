import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {}, publishDiagnostics: { relatedInformation: true } },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// A saved header asks the client to pull again the files including it: no
// edit of theirs would.
test(async (t) => {
    const s = await t.start({ capabilities: PULL });
    await t.show("before", s.diagnostics("main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    s.open("header.h");
    s.edit("header.h", { replace: "return 1;", with: "return missing;" });
    s.save("header.h");
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await t.show("after", s.diagnostics("main.cpp"));
});
