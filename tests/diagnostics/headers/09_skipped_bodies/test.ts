import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const PULL = {
    textDocument: { diagnostic: {}, publishDiagnostics: { relatedInformation: true } },
    workspace: { diagnostics: { refreshSupport: true } },
};
const REFRESH = "workspace/diagnostic/refresh";

// The editor's compile of main.cpp skips the function bodies of shapes.h,
// which it includes at its top: what goes wrong in area()'s body,
// instantiated for Square, and in the lambda apply()'s body calls shows
// once the background index has compiled main.cpp as saved, while the
// buffer holds that text.
test("as saved", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("indexed", s.diagnostics("main.cpp"), { parts: ["related"] });
    s.edit("main.cpp", { replace: "Square{2}", with: "Square{3}" });
    await t.show("edited", s.diagnostics("main.cpp"));
    s.save("main.cpp");
    await s.indexed();
    await t.show("saved", s.diagnostics("main.cpp"));
});

// An editor of the pull model holding the edited text's diagnostics is
// asked to pull again when the run reports; one of the push model gets a
// push of its own.
test("pulled", async (t) => {
    const s = await t.start({ capabilities: PULL });
    await s.indexed();
    await s.diagnostics("main.cpp");
    s.edit("main.cpp", { replace: "Square{2}", with: "Square{3}" });
    await s.diagnostics("main.cpp");
    const refreshes = await s.serverRequests(REFRESH);
    s.save("main.cpp");
    await s.indexed();
    expect(await s.serverRequests(REFRESH)).toBe(refreshes + 1);
});

test("pushed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await s.diagnostics("main.cpp");
    s.edit("main.cpp", { replace: "Square{2}", with: "Square{3}" });
    await s.diagnostics("main.cpp");
    s.save("main.cpp");
    await s.indexed();
    await t.show("pushes", s.pushes("main.cpp"));
});

// A header saved with an error in a function body reaches the open file
// including it through that file's background run, and so does its fix.
test("a saved header", async (t) => {
    const s = await t.start();
    await s.indexed();
    await s.diagnostics("main.cpp");
    s.open("shapes.h");
    s.edit("shapes.h", { replace: "return 1;", with: "return missing;" });
    s.save("shapes.h");
    await s.indexed();
    await t.show("saved header", s.diagnostics("main.cpp"));
    s.edit("shapes.h", { replace: "return missing;", with: "return 1;" });
    s.save("shapes.h");
    await s.indexed();
    await t.show("fixed header", s.diagnostics("main.cpp"));
});

// The index of the last session is current: main.cpp, opened again, runs
// for what the bodies hold all the same.
test("after a restart", async (t) => {
    const s = await t.start();
    await s.indexed();
    await s.restart();
    s.open("main.cpp");
    await s.indexed();
    await t.show("restarted", s.diagnostics("main.cpp"));
});

// Turned off with the cache of a session that skipped them, the editor
// parses the bodies.
test("bodies parsed", async (t) => {
    const config = { project: { enable_indexing: false } };
    const skipping = await t.start({ config });
    await skipping.diagnostics("main.cpp");
    await skipping.stop();
    const s = await t.start({
        config: { project: { ...config.project, skip_header_bodies: false } },
    });
    await t.show("parsed", s.diagnostics("main.cpp"));
});
