import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

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

// The tokens of a range are those of the whole file it overlaps, whole; of a
// comment that spans lines, the lines it overlaps.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const range = { start: { line: 1, character: 6 }, end: { line: 4, character: 0 } };
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
});
