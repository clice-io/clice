import * as path from "node:path";

import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// The user's file, under the directory of the copy XDG_CONFIG_HOME names.
function env(t: TestContext): Record<string, string> {
    return { XDG_CONFIG_HOME: path.join(t.root, "user") };
}

// A cache directory the user's file names for every project serves the
// first only: the other falls back to its own, and is told how each gets
// one.
test("one directory", async (t) => {
    t.disk.write("user/clice/config.toml", 'cache_dir = "../shared/.clice"\n');
    await t.cli("index --workspace a --jobs 1", { env: env(t) });
    await t.cli("index --workspace b --jobs 1", { env: env(t) });
    expect(t.disk.list("b/.clice/**").length).toBeGreaterThan(0);
});

// With ${workspace_id}, each project has a directory of its own under it.
test("a directory each", async (t) => {
    t.disk.write(
        "user/clice/config.toml",
        'cache_dir = "../caches/${workspace_id}/.clice"\n',
    );
    await t.cli("index --workspace a --jobs 1", { env: env(t) });
    await t.cli("index --workspace b --jobs 1", { env: env(t) });
    const caches = new Set(t.disk.list("caches/*/.clice/**").map((file) => file.split("/")[1]));
    expect([...caches].sort()).toEqual([
        expect.stringMatching(/^a-[0-9a-f]{8}$/),
        expect.stringMatching(/^b-[0-9a-f]{8}$/),
    ]);
});
