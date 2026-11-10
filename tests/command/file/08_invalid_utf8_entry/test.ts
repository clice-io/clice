import { test } from "@clice/tools/testing";

const UNLISTED = "No compilation database lists";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// The database is Latin-1, as a build writes a legacy -D value: its é is no
// UTF-8. Only a.cpp's entry holds it; b.cpp's is plain ASCII.
test({ errors: [UNLISTED, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command a.cpp");
    await t.cli("command b.cpp");
    await t.cli("command convert compile_commands.json");
});
