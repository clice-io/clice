import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const SKIPPED = { rules: [{ patterns: ["third_party/**"], skip_function_body: true }] };

// main.cpp instantiates size_of with a type it does not fit, and lib's
// twice() calls helper(): the error shows on main.cpp, the calls among
// helper's references.
test("bodies compiled", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await s.indexed();
    await t.show("references", s.references(t.at("main.cpp", "int |helper(int x) {")));
});

// A rule skipping the bodies of third_party/ skips them in lib's PCM too,
// which main.cpp still needs: what goes wrong in size_of's body never shows,
// and the index holds lib's declarations without what twice() calls.
test("bodies skipped", async (t) => {
    const s = await t.start({ config: SKIPPED });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await s.indexed();
    await t.show("references", s.references(t.at("main.cpp", "int |helper(int x) {")));
    await t.show("lib's declarations", s.workspaceSymbols("twice"));
});

// The PCM a session built with lib's bodies serves no session skipping
// them: the next one builds its own.
test("rule added across a restart", async (t) => {
    const first = await t.start();
    await first.diagnostics("main.cpp");
    await first.stop();
    const s = await t.start({ config: SKIPPED });
    await t.show("skipped", s.diagnostics("main.cpp"));
    expect((await s.counts()).pcm).toBe(1);
});
