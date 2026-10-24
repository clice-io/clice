import { describe, expect, test } from "vitest";
import { KEYWORDS, lex } from "./lex.ts";

/// The identifiers outside directives, as the sweep picks its places
/// among them.
function codeIdentifiers(text: string): string[] {
    return lex(text)
        .filter((token) => token.kind === "ident" && !token.directive)
        .map((token) => text.slice(token.begin, token.end))
        .filter((word) => !KEYWORDS.has(word));
}

describe("lex", () => {
    test("comments, literals and directives hold no code identifiers", () => {
        const text =
            '/* Copyright (C) 2026 */\n// Foo(bar)\n#define F(x) \\\n    g(x)\nint main() { return h(R"(a(b)"); }\n';
        expect(codeIdentifiers(text)).toEqual(["main", "h"]);
    });

    test("an unterminated literal ends with its line", () => {
        expect(codeIdentifiers("#error can't build\nint f();\n")).toEqual(["f"]);
        expect(codeIdentifiers("#if 0\ndon't\n#endif\nint g;\n")).toEqual(["don", "g"]);
    });
});
