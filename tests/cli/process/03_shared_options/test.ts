import { test } from "@clice/tools/testing";

// Options most commands share: a log level, a workspace.

test("log level", async (t) => {
    await t.cli("index --log-level bogus");
    await t.cli("index --log-level error --workers 1");
});

test("a missing workspace", async (t) => {
    for (const command of [
        "query --method projectFiles",
        "format --check",
        "lint",
        "index",
        "scan",
    ]) {
        await t.cli(`${command} --workspace missing`);
    }
    // What the runs left where the workspace was named.
    await t.show("left behind", t.disk.list("missing/.clice/*"));
});
