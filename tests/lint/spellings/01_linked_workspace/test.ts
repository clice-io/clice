import { test } from "@clice/tools/testing";

// Pins the workspace reached through a link: findings, notes included, name
// their files under the workspace as the command line spells it.
test(
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("ws", "real");
        await t.cli("lint --workspace ws --workers 2");
    },
);
