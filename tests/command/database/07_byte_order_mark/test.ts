import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// A database saved as UTF-8 with a byte order mark, as Windows tools write
// it, reads like any other.
test({ errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    t.disk.write("compile_commands.json", `\ufeff${t.disk.read("compile_commands.json")}`);
    await t.cli("command convert compile_commands.json");
});
