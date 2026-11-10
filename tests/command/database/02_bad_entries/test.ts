import { test } from "@clice/tools/testing";

const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// An entry missing its directory, file or command, an empty command, one
// naming an empty program and an item that is no entry are skipped; the
// good entries around them load, one whose command starts with a blank
// among them.
test({ errors: [UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
