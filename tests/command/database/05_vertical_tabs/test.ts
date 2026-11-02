import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// Spaces and tabs separate a command line's arguments, and on POSIX hosts
// carriage returns and newlines too, as a shell splits it; a vertical tab
// and a form feed stay inside the argument.
test({ errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
