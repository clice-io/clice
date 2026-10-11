import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// One file open under two names: while the texts agree the second name
// shares the first's answer, once they part it gets a note instead, and
// when the first closes it compiles on its own.
test({ except: { win32: "symbolic links need privileges" } }, async (t) => {
    t.disk.symlink("link", "real");
    const s = await t.start({ capabilities: PULL });
    s.open("real/main.cpp");
    s.open("link/main.cpp");
    await t.show("equal texts", s.diagnostics("link/main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    s.edit("real/main.cpp", { replace: "missing", with: "0" });
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await t.show("texts part", s.diagnostics("link/main.cpp"));
    s.close("real/main.cpp");
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 2);
    await t.show("second name alone", s.diagnostics("link/main.cpp"));
    await t.show("pushes of the first", s.pushes("real/main.cpp"));
    await t.show("pushes of the second", s.pushes("link/main.cpp"));
});
