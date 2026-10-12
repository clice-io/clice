import { test } from "@clice/tools/testing";

// A configuration or a database that cannot be loaded fails the run with
// exit 2: the rules keeping files out would be gone, and the build's file
// set incomplete. Nothing is rewritten.

test("a broken configuration", async (t) => {
    t.disk.write("clice.toml", "[[rules\npatterns = [\n");
    await t.cli("format");
});

test("a broken compilation database", async (t) => {
    t.disk.database();
    t.disk.write("compile_commands.json", "{");
    await t.cli("format --check");
});
