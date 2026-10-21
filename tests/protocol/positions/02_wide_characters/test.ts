import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Positions count UTF-16 code units, as LSP asks of a client that offers no
// other encoding, and UTF-8 bytes for a client that offers those: characters
// outside ASCII before a name on its line, one of them outside the Basic
// Multilingual Plane, move nothing either way. The warning comes from the
// preamble, which the precompiled header keeps; a note points into a header
// no editor opened; the edit travels as its range.

/// Each token of an encoded answer, at its absolute line and column.
function decode(data: readonly number[]): number[][] {
    const out: number[][] = [];
    let line = 0;
    let character = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        const [deltaLine = 0, deltaStart = 0, ...rest] = data.slice(i, i + 5);
        line += deltaLine;
        character = deltaLine === 0 ? character + deltaStart : deltaStart;
        out.push([line, character, ...rest]);
    }
    return out;
}

for (const encoding of ["utf-16", "utf-8"] as const) {
    test(encoding, async (t) => {
        const s = await t.start(encoding === "utf-8" ? { positionEncoding: encoding } : {});
        await t.show("diagnostics", s.diagnostics("main.cpp"), { parts: ["related"] });
        await t.show("hover", s.hover(t.marker("sum")));
        await t.show("definition", s.definition(t.marker("call")));
        await t.show("highlights", s.highlights(t.marker("sum")));
        await t.show("tokens", s.semanticTokens("main.cpp"));
        await t.show("hints", s.inlayHints("main.cpp"));

        // The tokens of a range starting after the wide characters are
        // those of the whole file it overlaps, counted in the same units.
        const range = s.range(t.at("main.cpp", "int sum = add(1, 2)"));
        const part = await s.request<proto.SemanticTokens>(
            "textDocument/semanticTokens/range",
            "main.cpp",
            { range },
        );
        const full = await s.request<proto.SemanticTokens>(
            "textDocument/semanticTokens/full",
            "main.cpp",
        );
        const at = (line: number, character: number) => line * 1000 + character;
        const inside = decode(full.data).filter(
            ([line = 0, character = 0, length = 0]) =>
                at(line, character + length) > at(range.start.line, range.start.character) &&
                at(line, character) < at(range.end.line, range.end.character),
        );
        expect(decode(part.data)).toEqual(inside);

        s.edit("main.cpp", { replace: "missing; int bad = twice();", with: "add(3, 4);" });
        await t.show("edited", s.diagnostics("main.cpp"));
        await t.show("edited hover", s.hover(t.at("main.cpp", "add(3, |4)")));
    });
}
