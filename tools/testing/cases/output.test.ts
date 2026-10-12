import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, test, vi } from "vitest";
import { Place, type Table } from "../render/value.ts";
import type { Case } from "./case.ts";
import { Output, checkOutput, readSnapshot, statusOf, writeSnapshot } from "./output.ts";

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

    test("flags survive a check and an update", () => {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-output-"));
        try {
            fs.writeFileSync(path.join(dir, "snap.toml"), flagged);
            const c = { id: "topic/sub/01_case", dir } as Case;
            const run = (b: Table) =>
                new Map([
                    [
                        "answers",
                        new Map([
                            ["a", { hover: {} }],
                            ["b", b],
                        ]),
                    ],
                ]);
            expect(() => checkOutput(c, run({ hover: {} }))).not.toThrow();
            vi.stubEnv("UPDATE_SNAPSHOTS", "1");
            checkOutput(c, run({ text: "x" }));
            expect(fs.readFileSync(path.join(dir, "snap.toml"), "utf8")).toBe(
                [
                    "[answers.a]",
                    "hover = {}",
                    "",
                    "[answers.b]",
                    'wrong = "not yet"',
                    'text = "x"',
                    "",
                ].join("\n"),
            );
        } finally {
            vi.unstubAllEnvs();
            fs.rmSync(dir, { recursive: true, force: true });
        }
    });
});
