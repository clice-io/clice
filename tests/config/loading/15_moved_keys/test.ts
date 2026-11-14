import { test } from "@clice/tools/testing";

// The keys of the layout before clice 1.0 are errors, each naming where it
// went, in the file and in the editor's settings alike: a configuration
// moved by hand cannot half apply.
test("in the file", async (t) => {
    const s = await t.start();
    await t.show("clice.toml", s.pushes("clice.toml"));
});

test("on the command line", { errors: ["in clice 1.0 at project"] }, async (t) => {
    await t.cli("index");
});

test("in the editor's settings", async (t) => {
    t.disk.rm("clice.toml");
    const s = await t.start({ config: { project: { readonly: "on" } } });
    await t.show("guidance", await s.guidance());
});
