import { describe, expect, test } from "vitest";
import { Output, applyFlags, assemble, blocks, flags, formatBlock, statusOf } from "./output.ts";

describe("Output", () => {
    test("blocks indent their body, one line stays on the label", () => {
        const output = new Output();
        output.block("after", "int");
        output.block("before", ["(none)", "  more"]);
        output.transcript("$ clice index", ["exit 0"]);
        expect(output.lines).toEqual([
            "after: int",
            "before",
            "  (none)",
            "    more",
            "$ clice index",
            "  exit 0",
        ]);
        expect(() => {
            output.block("after", "long");
        }).toThrow(/two blocks/);
    });

    test("labels are unique in their file's section", () => {
        const output = new Output();
        output.blocks(["--- a.cpp", "nameless_0: x", "", "--- b.cpp", "nameless_0: y"]);
        expect(() => {
            output.blocks(["--- a.cpp", "nameless_0: z"]);
        }).toThrow(/two blocks/);
    });

    test("labels are names", () => {
        expect(formatBlock("hover", [])).toEqual(["hover: (none)"]);
        for (const label of ["a: b", "$ clice", "--- x", "=== y", " lead", ""]) {
            expect(() => formatBlock(label, "x")).toThrow(/no label/);
        }
    });

    test("tests head their sections when there are several", () => {
        expect(assemble([{ name: "", lines: ["a: 1"] }])).toBe("a: 1\n");
        expect(
            assemble([
                { name: "one", lines: ["a: 1"] },
                { name: "two", lines: ["b: 2"] },
            ]),
        ).toBe("=== one\na: 1\n\n=== two\nb: 2\n");
    });

    test("blocks key by test, file and label", () => {
        const text =
            "=== edit\n--- main.cpp\ncall\n  definition: x\n$ clice index\n  exit 0\n$ clice index\n  exit 1\n";
        expect([...blocks(text).keys()]).toEqual([
            "edit ▸ main.cpp ▸ call",
            "edit ▸ main.cpp ▸ $ clice index",
            "edit ▸ main.cpp ▸ $ clice index #2",
        ]);
    });
});

describe("known wrong", () => {
    const flagged = "a: 1\nb  ✗ known wrong: not yet\n  2\nc\n  3\n";

    test("flags survive a new rendering", () => {
        expect([...flags(flagged)]).toEqual([["b", "not yet"]]);
        expect(applyFlags("a: 1\nb: 4\nc\n  3\n", flags(flagged))).toBe(
            "a: 1\nb  ✗ known wrong: not yet\n  4\nc\n  3\n",
        );
        expect(blocks(flagged).get("b")).toBe("2");
    });

    test("a hand-written flag on one line reads the same", () => {
        expect([...flags("b: 2  ✗ known wrong: not yet\n")]).toEqual([["b", "not yet"]]);
    });

    test("status follows the flags", () => {
        expect(statusOf("a: 1\n")).toBe("supported");
        expect(statusOf(flagged)).toBe("partial");
        expect(statusOf("b  ✗ known wrong: no\n  2\n")).toBe("unsupported");
    });
});
