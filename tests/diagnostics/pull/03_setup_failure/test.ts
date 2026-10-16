import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {} },
    workspace: {
        diagnostics: { refreshSupport: true },
        semanticTokens: { refreshSupport: true },
    },
};
const REFRESHES = ["workspace/diagnostic/refresh", "workspace/semanticTokens/refresh"];

// Every request tries the setup again and fails the same way: no refresh
// follows, which would make the client ask, try and fail in a loop.
test(async (t) => {
    const s = await t.start({ capabilities: PULL });
    await t.show("pulled", s.diagnostics("main.cpp"));
    const refreshes = await Promise.all(REFRESHES.map((method) => s.serverRequests(method)));
    await t.show("hover", s.hover(t.at("main.cpp", "int |main")));
    await t.show("pulled again", s.diagnostics("main.cpp"));
    expect(await Promise.all(REFRESHES.map((method) => s.serverRequests(method)))).toEqual(
        refreshes,
    );
});
