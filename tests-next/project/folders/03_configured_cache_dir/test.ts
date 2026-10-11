import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// Whether the directory holds any file.
function exists(t: TestContext, dir: string): boolean {
    return t.disk.list(`${dir}/**`).length > 0;
}

test("configured cache directory serves once", async (t) => {
    // Every folder's clice.toml names one directory.
    const s = await t.start({ folders: ["alpha", "beta", "gamma"] });
    s.open("alpha/main.cpp");
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("_fn"));
    await t.show("guidance", await s.guidance());
    await s.stop();
    // alpha takes the client's directory and beta its clice.toml's; gamma
    // finds both taken and falls back to its default.
    expect(exists(t, ".clice")).toBe(true);
    expect(exists(t, "shared")).toBe(true);
    expect(exists(t, "gamma/.clice")).toBe(true);
    expect(exists(t, "alpha/.clice")).toBe(false);
    expect(exists(t, "beta/.clice")).toBe(false);
});
