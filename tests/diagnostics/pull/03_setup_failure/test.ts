import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// Every request tries the setup again and fails the same way: no refresh
// follows, which would make the client pull, try and fail in a loop.
test(async (t) => {
    const s = await t.start({ capabilities: PULL });
    await t.show("pulled", s.diagnostics("main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    await t.show("hover", s.hover(t.at("main.cpp", "int |main")));
    await t.show("pulled again", s.diagnostics("main.cpp"));
    expect(await s.serverRequests(REFRESH)).toBe(refreshes);
});
