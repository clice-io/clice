import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// The header changes on disk and a request recompiles main.cpp: its pulled
// answer went stale with no edit to make the client pull again, so the
// server asks for a refresh.
test(async (t) => {
    const s = await t.start({ capabilities: PULL });
    await t.show("before", s.diagnostics("main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    const written = t.disk.mtime("header.h");
    t.disk.write("header.h", "inline int value() { return missing; }\n");
    t.disk.touch("header.h", new Date(written.getTime() + 10_000));
    await s.hover(t.at("main.cpp", "return |value()"));
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await t.show("after", s.diagnostics("main.cpp"));
    await t.show("pushes", s.pushes("main.cpp"));
});
