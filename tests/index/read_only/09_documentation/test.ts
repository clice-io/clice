import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const markers = [
    "call",
    "field",
    "override",
    "macro",
    "variable",
    "absolute",
    "overload",
    "nested",
    "template",
    "parameter",
];

// Read-only, the cards come from the index: the comment documenting the
// declaration in the header, not the one above the definition elsewhere.
test("index cards carry documentation", async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    for (const name of markers) {
        await t.show(name, s.hover(t.marker(name)));
    }
    expect((await s.counts()).pch).toBe(0);
});

test("index cards follow the options", async (t) => {
    const s = await t.start({
        config: { project: { readonly: "on" }, documentation: { format: "plain", inherit: false } },
    });
    s.open("main.cpp", { pull: false });
    await s.sync();
    for (const name of ["call", "override"]) {
        await t.show(name, s.hover(t.marker(name)));
    }
});

test("plain text client", async (t) => {
    const s = await t.start({
        config: { project: { readonly: "on" } },
        capabilities: { textDocument: { hover: { contentFormat: ["plaintext"] } } },
    });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("call", s.hover(t.marker("call")));
});
