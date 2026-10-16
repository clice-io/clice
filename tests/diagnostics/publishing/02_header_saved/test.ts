import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PUSH = {
    workspace: { semanticTokens: { refreshSupport: true } },
    textDocument: { publishDiagnostics: { relatedInformation: true } },
};
const REFRESH = "workspace/semanticTokens/refresh";

// A client without pull support: a saved header asks it to request again
// what it shows of the files including it, and the request it sends, as an
// editor does for the files on screen, compiles main.cpp against the saved
// header and publishes its diagnostics.
test(async (t) => {
    const s = await t.start({ capabilities: PUSH });
    s.open("main.cpp");
    await t.show("opened", s.pushes("main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    s.open("header.h");
    s.edit("header.h", { replace: "return 1;", with: "return missing;" });
    s.save("header.h");
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await s.semanticTokens("main.cpp");
    await t.show("saved", s.pushes("main.cpp"));
});
