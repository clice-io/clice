import { answers, test } from "@clice/tools/testing";

answers("default", async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});

test("plain text client", async (t) => {
    const s = await t.start({
        capabilities: { textDocument: { hover: { contentFormat: ["plaintext"] } } },
    });
    s.open("main.cpp");
    await s.sync();
    for (const name of ["function", "variable", "field"]) {
        await t.show(name, s.hover(t.marker(name)));
    }
});
