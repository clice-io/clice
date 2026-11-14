import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A file outside both folders compiles with the default command, whichever
// folder the client listed first: it borrows neither folder's database.
test("alpha first", async (t) => {
    const s = await t.start({ folders: ["alpha", "beta"] });
    await t.show("stray", s.diagnostics("elsewhere/stray.cpp"));
});

test("beta first", async (t) => {
    const s = await t.start({ folders: ["beta", "alpha"] });
    await t.show("stray", s.diagnostics("elsewhere/stray.cpp"));
});

// Listed in alpha's database, the file compiles as alpha's; dropped from it,
// the file moves to the files outside every folder.
test("dropped from a database", async (t) => {
    t.disk.database(
        { "alpha/main.cpp": ["-DIN_ALPHA"], "elsewhere/stray.cpp": ["-DIN_ALPHA"] },
        "alpha",
    );
    const s = await t.start({ folders: ["alpha", "beta"] });
    await t.show("listed", s.diagnostics("elsewhere/stray.cpp"));
    t.disk.database({ "alpha/main.cpp": ["-DIN_ALPHA"] }, "alpha");
    expect(await s.poll("cdb")).toBe(1);
    await t.show("dropped", s.diagnostics("elsewhere/stray.cpp"));
});

// The editor's rules, and the configuration they declare, are the folders':
// a file outside every folder compiles without them.
test("editor rules with a configuration", async (t) => {
    const s = await t.start({
        folders: ["alpha", "beta"],
        config: {
            default_configuration: "debug",
            rules: [{ configuration: "debug", append: ["-DIN_ALPHA"] }],
        },
    });
    await t.show("stray", s.diagnostics("elsewhere/stray.cpp"));
    await t.show("guidance", await s.guidance());
});
