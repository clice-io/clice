import { test } from "@clice/tools/testing";

// Pins the workspace reached through a link: findings, notes included, name
// their files under the workspace as the command line spells it.
test(async (t) => {
    t.disk.symlink("ws", "real");
    await t.cli("lint --workspace ws --jobs 2");
});
