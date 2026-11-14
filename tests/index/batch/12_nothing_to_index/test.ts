import { test } from "@clice/tools/testing";

// A directory with nothing to index: the run says so before it writes
// anything, and leaves no cache directory or session log behind.
test({ errors: ["Nothing to index: no compile_commands.json found"] }, async (t) => {
    await t.cli("index");
    await t.show("created", t.disk.list(".clice/**"));
});
