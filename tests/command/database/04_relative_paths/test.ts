import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";

// An entry's file is relative to its directory: the same spelling in two
// entries names two files, and none at the root. A path through . and ..
// names the file it reaches, from a directory the build has not made yet
// too.
test({ errors: [MISSING, "No compilation database lists"] }, async (t) => {
    await t.cli("command project/build/src/main.cpp");
    await t.cli("command other/build/src/main.cpp");
    await t.cli("command src/main.cpp");
    await t.cli("command project/src/main.cpp");
});
