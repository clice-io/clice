import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNRESOLVED = "of the converted entries could not be resolved";

// Entries written as an argument list and as one command line sit side by
// side; a command line splits as a shell would, quotes keeping spaces
// inside one argument.
test({ errors: [MISSING, UNRESOLVED] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
