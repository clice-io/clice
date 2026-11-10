import { test, type TestContext } from "@clice/tools/testing";

async function answers(t: TestContext, host: string): Promise<void> {
    t.disk.database({ [host]: [] });
    const s = await t.start({ capabilities: { textDocument: { inlayHint: {} } } });
    await s.indexed();
    await s.errors("real/checks.h");
    for (const m of ["macro", "function", "alias", "type"]) {
        await t.show(m, s.definition(t.marker(m)));
    }
    await t.show("hints", s.inlayHints("real/checks.h"), { parts: ["links"] });
}

test("direct", (t) => answers(t, "real/main.cpp"));

test("through a link", async (t) => {
    t.disk.symlink("link", "real");
    await answers(t, "link/main.cpp");
});
