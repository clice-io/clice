import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {} },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

test("the client's capability picks the model", async (t) => {
    const pulling = await t.start({ capabilities: PULL });
    // What the server advertises is no request a client renders.
    expect(pulling.client.initResult?.capabilities.diagnosticProvider).toEqual({
        interFileDependencies: false,
        workspaceDiagnostics: false,
    });
    await t.show("pulled", pulling.diagnostics("main.cpp"));
    await t.show("pushes to a pulling client", pulling.pushes("main.cpp"));
    await pulling.stop();
    const pushed = await t.start();
    expect(pushed.client.initResult?.capabilities.diagnosticProvider).toBeUndefined();
    pushed.open("main.cpp");
    await t.show("pushes to another client", pushed.pushes("main.cpp"));
});

// The client pulls after its own edits: no refresh is owed for them.
test("pulls follow edits", async (t) => {
    const s = await t.start({ capabilities: PULL });
    await t.show("first", s.diagnostics("main.cpp"));
    const refreshes = await s.serverRequests(REFRESH);
    s.edit("main.cpp", { replace: "missing", with: "0" });
    await t.show("fixed", s.diagnostics("main.cpp"));
    s.edit("main.cpp", { replace: "return 0", with: "return second" });
    await t.show("broken again", s.diagnostics("main.cpp"));
    expect(await s.serverRequests(REFRESH)).toBe(refreshes);
    await t.show("pushes", s.pushes("main.cpp"));
});

test("an edit mid-pull answers the new text", async (t) => {
    const s = await t.start({ capabilities: PULL });
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    const pulled = s.diagnostics("main.cpp");
    await park.reached();
    s.edit("main.cpp", { replace: "missing", with: "second" });
    await park.release();
    await t.show("pulled", pulled);
});

// The cancelled pull answers at once; the next still waits on the compile
// the cancelled one left.
test("a cancelled pull answers at once", async (t) => {
    const s = await t.start({ capabilities: PULL });
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("main.cpp", { pull: false });
    const first = s.send("textDocument/diagnostic", "main.cpp");
    await park.reached();
    first.cancel();
    let answered = false;
    const next = s.diagnostics("main.cpp");
    void next.then(() => {
        answered = true;
    });
    await expect(first.reply).rejects.toMatchObject({
        code: proto.LSPErrorCodes.RequestCancelled,
    });
    expect(answered).toBe(false);
    await park.release();
    await t.show("next", next);
});

test("a closed document pulls empty", async (t) => {
    const s = await t.start({ capabilities: PULL });
    await t.show("open", s.diagnostics("main.cpp"));
    s.close("main.cpp");
    expect(await s.request("textDocument/diagnostic", "main.cpp")).toEqual({
        kind: "full",
        items: [],
    });
    await t.show("pushes", s.pushes("main.cpp"));
});
