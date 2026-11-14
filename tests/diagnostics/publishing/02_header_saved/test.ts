import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PUSH = {
    workspace: { semanticTokens: { refreshSupport: true } },
    textDocument: { publishDiagnostics: { relatedInformation: true } },
};
const REFRESH = "workspace/semanticTokens/refresh";

// A client without pull support: a saved header asks it to request again
// what it shows of the files including it, and compiles main.cpp against
// the saved header, which publishes its diagnostics with no request.
test(async (t) => {
    const s = await t.start({ capabilities: PUSH });
    s.open("main.cpp");
    await t.show("opened", s.pushes("main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    s.open("header.h");
    s.edit("header.h", { replace: "return 1;", with: "return missing;" });
    s.save("header.h");
    // One for the change, one once main.cpp compiled against it.
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 2);
    await t.show("saved", s.pushes("main.cpp"));
});
