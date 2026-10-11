import { test } from "@clice/tools/testing";

// Pins the runs that cannot check anything: no database under the
// workspace, a workspace that does not exist, a worker count that is not a
// number.
test("no database", { errors: ["Nothing to lint: no compile_commands.json found"] }, async (t) => {
    await t.cli("lint --workers 2");
});

test("missing workspace", async (t) => {
    await t.cli("lint --workspace nowhere --workers 2");
    await t.show("created", t.disk.list("nowhere/.clice/CACHEDIR.TAG"));
});

test("invalid worker count", async (t) => {
    await t.cli("lint --workers many");
});
