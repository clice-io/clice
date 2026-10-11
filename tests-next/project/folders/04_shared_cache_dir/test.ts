import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// Whether the directory holds any file.
function exists(t: TestContext, dir: string): boolean {
    return t.disk.list(`${dir}/**`).length > 0;
}

test("shared cache directory serves one project", async (t) => {
    // Both clice.toml name shared/.clice.
    await t.cli("index --workspace a --workers 1");
    // Run after it, the other project indexes into a cache of its own
    // instead of the first one's.
    await t.cli("index --workspace b --workers 1");
    expect(exists(t, "shared/.clice")).toBe(true);
    expect(exists(t, "a/.clice")).toBe(false);
    expect(exists(t, "b/.clice")).toBe(true);
});
