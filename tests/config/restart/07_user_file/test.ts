import * as path from "node:path";

import { test } from "@clice/tools/testing";

// The user's file, here under a directory of the copy XDG_CONFIG_HOME
// names, is every project's: one written, changed or deleted while the
// server runs restarts it.
test("written", async (t) => {
    const s = await t.start({ env: { XDG_CONFIG_HOME: path.join(t.root, "user") } });
    await s.sync();
    t.disk.write("user/clice/config.toml", "[workers]\nforeground = 1\n");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});

test("changed", async (t) => {
    t.disk.write("user/clice/config.toml", "[workers]\nforeground = 1\n");
    const s = await t.start({ env: { XDG_CONFIG_HOME: path.join(t.root, "user") } });
    await s.sync();
    t.disk.write("user/clice/config.toml", "[workers]\nforeground = 2\n");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});

test("deleted", async (t) => {
    t.disk.write("user/clice/config.toml", "[workers]\nforeground = 1\n");
    const s = await t.start({ env: { XDG_CONFIG_HOME: path.join(t.root, "user") } });
    await s.sync();
    t.disk.rm("user/clice/config.toml");
    await s.poll("workspace");
    await t.show("asked", await s.restarted());
});
