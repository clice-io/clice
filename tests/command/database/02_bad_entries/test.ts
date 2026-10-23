import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";
const SKIPPED = "Skipping item at index";

// An entry missing its directory, file or command, an empty command and
// an item that is no entry are skipped; the good entries around them load.
test({ errors: [SKIPPED, MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
