import { test } from "@clice/tools/testing";

// The build assembles entry.S after preprocessing it and boot.s as it is:
// neither is C-family code, so clice indexes, lints and serves only main.c.

test("open", async (t) => {
    const s = await t.start();
    await t.show("preprocessed", s.diagnostics("entry.S"));
    s.edit("entry.S", { replace: "SIZE 8", with: "SIZE 16" });
    await t.show("edited", s.diagnostics("entry.S"));
    await t.show("plain", s.diagnostics("boot.s"));
});

test("inspect", async (t) => {
    await t.cli("inspect semantic-tokens entry.S");
});

test("index", async (t) => {
    await t.cli("index --jobs 1");
});

test("lint", async (t) => {
    await t.cli("lint");
});
