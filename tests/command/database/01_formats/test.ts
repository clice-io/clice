import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// Entries written as an argument list and as one command line sit side by
// side; a command line splits as a shell would, quotes keeping spaces
// inside one argument.
test({ errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
