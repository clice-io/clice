/// Tests for the semantic-tokens feature module: both adapters' piece
/// shapes, the marker-focus filter, and the multiline split the two paths
/// must agree on.

import { expect, test } from "vitest";
import { parseAnnotations } from "@clice/tools/snap/annotation";
import {
    decodeSemanticTokenPieces,
    focusSemanticTokens,
    rawSemanticTokenPieces,
    semanticTokenFocusOffsets,
} from "@clice/tools/snap/features/semantic_tokens";

test("semantic token decoding", () => {
    const legend = {
        tokenTypes: ["Type", "Function"],
        tokenModifiers: ["Definition", "Readonly"],
    };
    const data = [0, 4, 3, 0, 1, 1, 2, 4, 1, 3];
    const lines = ["abc defg", "xxfuncy"];
    expect(decodeSemanticTokenPieces(data, lines, legend).map((piece) => piece.rendered)).toEqual([
        '- { loc: "0:4", text: "def", kind: Type, modifiers: [Definition] }',
        '- { loc: "1:2", text: "func", kind: Function, modifiers: [Definition, Readonly] }',
    ]);
});

test("marker focus filters tokens and pins misses", () => {
    // A named and a nameless marker on the same offset collapse to one
    // entry; the marker on `=` (no token there) must render an explicit
    // `kind: none` line so the absence itself is snapshot content.
    const source = parseAnnotations("int §(v)§a §= done;\n");
    const stripped = Buffer.from(source.content);
    const raw = [
        { range: { begin: 0, end: 3 }, kind: "Keyword", modifiers: 0 },
        { range: { begin: 4, end: 5 }, kind: "Variable", modifiers: 0 },
    ];
    const offsets = semanticTokenFocusOffsets(source);
    expect(offsets).toEqual([4, 6]);
    const focused = focusSemanticTokens(rawSemanticTokenPieces(raw, stripped), offsets, stripped);
    expect(focused).toEqual([
        '- { loc: "0:4", text: "a", kind: variable }',
        '- { loc: "0:6", text: "=", kind: none }',
    ]);

    // A marker on an identifier no token covers keeps the identifier as
    // the miss label, so a later fix flips kind while the text stays put.
    const bare = parseAnnotations("goto §done;\n");
    const none = focusSemanticTokens(
        [],
        semanticTokenFocusOffsets(bare),
        Buffer.from(bare.content),
    );
    expect(none).toEqual(['- { loc: "0:5", text: "done", kind: none }']);

    // A miss on a non-ASCII position labels with the whole UTF-8 sequence,
    // not a lone byte turned into mojibake.
    const cjk = parseAnnotations("int x; // §你好\n");
    const cjkOffsets = semanticTokenFocusOffsets(cjk);
    expect(focusSemanticTokens([], cjkOffsets, Buffer.from(cjk.content))).toEqual([
        '- { loc: "0:10", text: "你", kind: none }',
    ]);

    // Range markers have no focus meaning; silently pinning the full dump
    // would hide the author's intent, so they are rejected.
    expect(() => semanticTokenFocusOffsets(parseAnnotations("§⟦int⟧ x;\n"))).toThrow(
        "point markers",
    );
});

test("marker focus agrees between inspect and server pieces", () => {
    const source = parseAnnotations("int §a = §1;\n");
    const stripped = Buffer.from(source.content);
    const raw = [
        { range: { begin: 0, end: 3 }, kind: "Keyword", modifiers: 0 },
        { range: { begin: 4, end: 5 }, kind: "Variable", modifiers: 0 },
        { range: { begin: 8, end: 9 }, kind: "Number", modifiers: 0 },
    ];
    const legend = { tokenTypes: ["keyword", "variable", "number"], tokenModifiers: [] };
    const decoded = decodeSemanticTokenPieces(
        [0, 0, 3, 0, 0, 0, 4, 1, 1, 0, 0, 4, 1, 2, 0],
        source.content.split("\n"),
        legend,
    );
    const offsets = semanticTokenFocusOffsets(source);
    const inspected = focusSemanticTokens(rawSemanticTokenPieces(raw, stripped), offsets, stripped);
    expect(inspected).toEqual(focusSemanticTokens(decoded, offsets, stripped));
    expect(inspected).toEqual([
        '- { loc: "0:4", text: "a", kind: variable }',
        '- { loc: "0:8", text: "1", kind: number }',
    ]);
});

test("multiline token split matches server decode", () => {
    // A token spanning a blank line: the server splits it per line and the
    // blank interior piece still encodes (its newline counts), so both
    // adapters must emit the empty-text entry identically.
    const content = "/*x\n\ny*/\nint a;\n";
    const raw = [{ range: { begin: 0, end: 8 }, kind: "Comment", modifiers: 0 }];
    const inspected = rawSemanticTokenPieces(raw, Buffer.from(content)).map(
        (piece) => piece.rendered,
    );
    const legend = { tokenTypes: ["comment"], tokenModifiers: [] };
    const decoded = decodeSemanticTokenPieces(
        [0, 0, 4, 0, 0, 1, 0, 1, 0, 0, 1, 0, 3, 0, 0],
        content.split("\n"),
        legend,
    ).map((piece) => piece.rendered);
    expect(inspected).toEqual(decoded);
    expect(inspected[1]).toBe('- { loc: "1:0", text: "", kind: comment }');
});
