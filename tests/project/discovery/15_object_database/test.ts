import { test } from "@clice/tools/testing";

// PowerShell's ConvertTo-Json writes a one-element array as the element
// itself: the database is one entry, not wrapped in [ ].
test(
    {
        errors: ["Nothing to lint: .*compile_commands.json cannot be read"],
    },
    async (t) => {
        await t.cli("lint");
    },
);
