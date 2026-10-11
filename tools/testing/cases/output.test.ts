import { describe, expect, test } from "vitest";
import { Place } from "../render/value.ts";
import { Output, readSnapshot, statusOf, writeSnapshot } from "./output.ts";

const where = {
    path: "main.cpp",
    range: { start: { line: 0, character: 4 }, end: { line: 0, character: 5 } },
};

describe("Output", () => {
    test("labels are unique, a command run again counts", () => {
        const output = new Output();
        output.block("after", { hover: {} });
        output.transcript("$ clice index", { exit: 0 });
        output.transcript("$ clice index", { exit: 1 });
        expect([...output.blocks.keys()]).toEqual(["after", "$ clice index", "$ clice index #2"]);
        expect(() => {
            output.block("after", { hover: {} });
        }).toThrow(/two blocks/);
    });
});

describe("snap.toml", () => {
    test("blocks are tables under their test", () => {
        const snapshot = new Map([
            [
                "answers",
                new Map([
                    [
                        "local",
                        {
                            highlights: [
                                { at: new Place("int ⟦x⟧ = 0;", where), kind: "text" },
                                { at: new Place('f("⟦x⟧");', where), kind: "read" },
                            ],
                        },
                    ],
                    ["hover while paused", { hover: {} }],
                ]),
            ],
        ]);
        expect(writeSnapshot(snapshot)).toBe(
            [
                "[answers.local]",
                "highlights = [",
                '    { at = "int ⟦x⟧ = 0;", kind = "text" },',
                `    { at = 'f("⟦x⟧");', kind = "read" },`,
                "]",
                "",
                '[answers."hover while paused"]',
                "hover = {}",
                "",
            ].join("\n"),
        );
    });

    test("text spanning lines stays raw", () => {
        const text = writeSnapshot(
            new Map([["test", new Map([["$ clice format", { stdout: 'say("\\n");\nnext' }]])]]),
        );
        expect(text).toBe(
            ['[test."$ clice format"]', "stdout = '''", 'say("\\n");', "next", "'''", ""].join(
                "\n",
            ),
        );
        expect(readSnapshot(text).get("test")?.get("$ clice format")).toEqual({
            stdout: 'say("\\n");\nnext',
        });
    });

    test("long lists take a line per item", () => {
        const items = Array.from(
            { length: 4 },
            (_, i) => `a fairly long location line ${String(i)}`,
        );
        expect(writeSnapshot(new Map([["t", new Map([["use", { definition: items }]])]]))).toBe(
            ["[t.use]", "definition = [", ...items.map((item) => `    "${item}",`), "]", ""].join(
                "\n",
            ),
        );
    });
});

describe("known wrong", () => {
    const flagged = [
        "[answers.a]",
        "hover = {}",
        "",
        "[answers.b]",
        'wrong = "not yet"',
        "hover = {}",
        "",
    ].join("\n");

    test("status follows the flags", () => {
        expect(statusOf("[answers.a]\nhover = {}\n")).toBe("supported");
        expect(statusOf(flagged)).toBe("partial");
        expect(statusOf('[answers.b]\nwrong = "no"\nhover = {}\n')).toBe("unsupported");
    });
});
