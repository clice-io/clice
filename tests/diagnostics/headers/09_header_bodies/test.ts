import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {}, publishDiagnostics: { relatedInformation: true } },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// What goes wrong in area()'s body, instantiated for Square, and in the
// lambda apply()'s body calls shows on main.cpp, edited or not.
test("as edited", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("opened", s.diagnostics("main.cpp"), { parts: ["related"] });
    s.edit("main.cpp", { replace: "Square{2}", with: "Square{3}" });
    await t.show("edited", s.diagnostics("main.cpp"));
});

// While clang instantiates, an editor of the pull model reads the parse's
// diagnostics and the file answers its requests; once the instantiations
// report, the editor is asked to pull again. One that cannot be asked waits
// for them.
test("while instantiating", async (t) => {
    const s = await t.start({
        capabilities: PULL,
        config: { project: { enable_indexing: false } },
    });
    await s.diagnostics("main.cpp");
    const refreshes = await s.serverRequests(REFRESH);
    const park = await s.park("finishCompile", "main.cpp", { at: "worker" });
    s.edit("main.cpp", { replace: "Square{2}", with: "Square{3}" });
    const parsed = s.diagnostics("main.cpp");
    await park.reached();
    await t.show("parsed", parsed);
    await t.show("hover", s.hover(t.at("main.cpp", "return ar|ea(Square{3})")));
    await park.release();
    await s.sync();
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
    await t.show("instantiated", s.diagnostics("main.cpp"));
});

// A header saved with an error in a function body reaches the open file
// including it, and so does its fix.
test("a saved header", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await s.diagnostics("main.cpp");
    s.open("shapes.h");
    s.edit("shapes.h", { replace: "return 1;", with: "return missing;" });
    s.save("shapes.h");
    await t.show("saved header", s.diagnostics("main.cpp"));
    s.edit("shapes.h", { replace: "return missing;", with: "return 1;" });
    s.save("shapes.h");
    await t.show("fixed header", s.diagnostics("main.cpp"));
});
