import { describe, expect, test } from "vitest";
import { applyEdits, renderDiff } from "./edits.ts";
import { SourceText } from "./position.ts";

const at = (line: number, character: number) => ({ line, character });

describe("applyEdits", () => {
    const source = new SourceText("a.cpp", "int a;\nint b;\n");

    test("edits at one place keep their order", () => {
        const here = { start: at(0, 0), end: at(0, 0) };
        expect(
            applyEdits(source, [
                { range: here, newText: "x" },
                { range: here, newText: "y" },
                { range: { start: at(1, 4), end: at(1, 5) }, newText: "c" },
            ]),
        ).toBe("xyint a;\nint c;\n");
    });

    test("overlapping edits are refused", () => {
        expect(() =>
            applyEdits(source, [
                { range: { start: at(0, 0), end: at(0, 5) }, newText: "" },
                { range: { start: at(0, 4), end: at(1, 0) }, newText: "" },
            ]),
        ).toThrow(/overlapping/);
    });
});

describe("renderDiff", () => {
    test("distant hunks are separated", () => {
        const before = Array.from({ length: 12 }, (_, i) => `line ${String(i)}`).join("\n") + "\n";
        const after = before.replace("line 1\n", "line one\n").replace("line 10\n", "line ten\n");
        expect(renderDiff(before, after).split("\n")).toEqual([
            " line 0",
            "-line 1",
            "+line one",
            " line 2",
            " line 3",
            "…",
            " line 8",
            " line 9",
            "-line 10",
            "+line ten",
            " line 11",
        ]);
    });

    test("whitespace changes show", () => {
        expect(renderDiff("int x;\n\nint y;  \n", "int x;\n\nint y;").split("\n")).toEqual([
            " int x;",
            "",
            "-int y;··",
            "+int y;",
            "\\ No newline at end of file",
        ]);
    });

    test("carriage returns show", () => {
        expect(renderDiff("int  x;\r\n\r\nint y;\r\n", "int x;\r\n\r\nint y;\r\n")).toBe(
            ["-int  x;␍", "+int x;␍", " ␍", " int y;␍"].join("\n"),
        );
    });

    test("no change says so", () => {
        expect(renderDiff("x\n", "x\n")).toBe("(no change)");
    });
});
