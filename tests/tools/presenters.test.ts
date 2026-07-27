/// Tests for the wire presenters (tools/snap/presenters.ts).

import { expect, test } from "vitest";
import { decodeSemanticTokens } from "@clice/tools/snap/presenters";

test("semantic token decoding", () => {
    const legend = {
        tokenTypes: ["Type", "Function"],
        tokenModifiers: ["Definition", "Readonly"],
    };
    const data = [0, 4, 3, 0, 1, 1, 2, 4, 1, 3];
    const lines = ["abc defg", "xxfuncy"];
    expect(decodeSemanticTokens(data, lines, legend)).toEqual([
        '- { loc: "0:4", text: "def", kind: Type, modifiers: [Definition] }',
        '- { loc: "1:2", text: "func", kind: Function, modifiers: [Definition, Readonly] }',
    ]);
});
