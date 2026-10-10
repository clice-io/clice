/// Assembler-with-cpp sources (`.S`): the preprocessor passes a `#` comment
/// line through to the assembler, and the editor must get past it (#791).

import { at, expect, serve } from "../../fixtures.ts";

serve.files(
    {
        "entry.S":
            "#define SIZE 8\n" +
            "# Copyright 2003 Pavel Machek <pavel@suse.cz\n" +
            ".text\n" +
            "# Hooray, we are in long mode\n" +
            "entry:\n" +
            "    .quad SIZE\n",
        "compile_commands.json": (workspace) =>
            JSON.stringify([
                {
                    directory: workspace.root,
                    file: workspace.path("entry.S"),
                    arguments: ["clang", "-c", workspace.path("entry.S"), "-o", "entry.o"],
                },
            ]),
    },
    { databases: false },
)("open assembler with comments", async ({ s }) => {
    await s.compiled("entry.S");

    const hover = await s.hover(at("entry.S", ".quad S|IZE"));
    expect(JSON.stringify(hover?.contents)).toContain("Expands to");
});
