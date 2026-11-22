import { describe, expect, test } from "vitest";
import { clean, isFinding, truncate } from "./bodies.ts";

describe("clean", () => {
    test("a codex finding keeps its text, place and severity", () => {
        const body = [
            "**<sub><sub>![P2 Badge](https://img.shields.io/badge/P2-yellow)</sub></sub>  Guard the empty range**",
            "",
            "`first` is read before the check in https://github.com/clice-io/clice/blob/0123abcd/src/a.cpp#L10-L12.",
            "",
            "Useful? React with 👍 / 👎.",
        ].join("\n");
        expect(clean(body)).toEqual({
            severity: "P2",
            text: "**Guard the empty range**\n\n`first` is read before the check in src/a.cpp:10-12.",
        });
    });

    test("noise blocks go, a per-file group unwraps", () => {
        const body = [
            "<details><summary>🤖 Prompt for AI Agents</summary>do this</details>",
            "<details>",
            "<summary>src/b.cpp (1)</summary>",
            "<details><summary>Walkthrough</summary>long</details>",
            "`src/b.cpp:4`: a leak",
            "</details>",
        ].join("\n");
        expect(clean(body).text).toBe("src/b.cpp (1)\n\n`src/b.cpp:4`: a leak");
    });

    test("markup inside code stays", () => {
        expect(clean("<p>use `std::vector<int>` here</p> and <b>x < 0</b>").text).toBe(
            "use `std::vector<int>` here and x < 0",
        );
    });

    test("a CodeRabbit level is the severity", () => {
        expect(clean("_⚠️ Potential issue_ | _🟠 Major_\n\nFix it.").severity).toBe("Major");
    });
});

describe("isFinding", () => {
    test("only a bot's located finding counts", () => {
        expect(isFinding("coderabbitai", "src/a.cpp:3 leaks")).toBe(true);
        expect(isFinding("coderabbitai", "Reviewed, nothing found")).toBe(false);
        expect(isFinding("someone", "src/a.cpp:3 leaks")).toBe(false);
    });
});

describe("truncate", () => {
    test("0 keeps everything", () => {
        expect(truncate("abcdef", 0)).toBe("abcdef");
        expect(truncate("abcdef", 4)).toBe("abc…");
    });
});
