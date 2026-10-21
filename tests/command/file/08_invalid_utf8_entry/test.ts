import { test } from "@clice/tools/testing";

const SKIPPED = "Skipping item at index 0";
const UNLISTED = "No compilation database lists";
const NOT_FOUND = "Toolchain query failed: Cannot find driver: missing-cc";
const UNRESOLVED = "of the converted entries could not be resolved";

// The database is Latin-1, as a build writes a legacy -D value: its é is no
// UTF-8. Only a.cpp's entry holds it; b.cpp's is plain ASCII.
test({ errors: [SKIPPED, UNLISTED, NOT_FOUND, UNRESOLVED] }, async (t) => {
    await t.cli("command a.cpp");
    await t.cli("command b.cpp");
    await t.cli("command convert compile_commands.json");
});
