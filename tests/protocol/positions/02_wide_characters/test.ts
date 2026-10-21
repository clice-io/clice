import { test } from "@clice/tools/testing";

// Positions count UTF-16 code units, as LSP asks of a client that offers no
// other encoding, and UTF-8 bytes for a client that offers those: characters
// outside ASCII before a name on its line, one of them outside the Basic
// Multilingual Plane, move nothing either way. The edit travels as its range.

for (const encoding of ["utf-16", "utf-8"] as const) {
    test(encoding, async (t) => {
        const s = await t.start(encoding === "utf-8" ? { positionEncoding: encoding } : {});
        await t.show("diagnostics", s.diagnostics("main.cpp"), { parts: ["related"] });
        await t.show("hover", s.hover(t.marker("sum")));
        await t.show("definition", s.definition(t.marker("call")));
        await t.show("highlights", s.highlights(t.marker("sum")));
        await t.show("tokens", s.semanticTokens("main.cpp"));
        await t.show("hints", s.inlayHints("main.cpp"));
        s.edit("main.cpp", { replace: "missing + add(3)", with: "add(3, 4)" });
        await t.show("edited", s.diagnostics("main.cpp"));
        await t.show("edited hover", s.hover(t.at("main.cpp", "add(3, |4)")));
    });
}
