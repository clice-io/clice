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
    },
    { manifest: { units: {} } },
)("open assembler with comments", async ({ s }) => {
    // The fixture's database spells every unit with clang++ -fsyntax-only.
    await s.offline(() => {
        const entry = s.workspace.path("entry.S");
        s.disk.write(
            "compile_commands.json",
            JSON.stringify([
                {
                    directory: s.workspace.root,
                    file: entry,
                    arguments: ["clang", "-c", entry, "-o", "entry.o"],
                },
            ]),
        );
    });
    await s.compiled("entry.S");

    const hover = await s.hover(at("entry.S", ".quad S|IZE"));
    expect(JSON.stringify(hover?.contents)).toContain("Expands to");
});
