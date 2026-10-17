import { describe, expect, test } from "vitest";
import { writeToml, type Table } from "./value.ts";

function write(table: Table): string[] {
    return writeToml([{ path: ["t"], table }]).split("\n");
}

const a = (n: number) => "a".repeat(n);

describe("snap.toml layout", () => {
    test("a list stays on a line of 120 columns, a longer one takes a line per item", () => {
        expect(write({ x: [a(112)] })).toEqual(["[t]", `x = ["${a(112)}"]`, ""]);
        expect(write({ x: [a(113)] })).toEqual(["[t]", "x = [", `    "${a(113)}",`, "]", ""]);
    });

    test("so does a table, its line's comma counted", () => {
        expect(write({ x: [{ a: a(105) }] })[2]).toBe(`    { a = "${a(105)}" },`);
        expect(write({ x: [{ a: a(106) }] })).toEqual([
            "[t]",
            "x = [",
            "    {",
            `        a = "${a(106)}",`,
            "    },",
            "]",
            "",
        ]);
    });

    test("a column is a grapheme", () => {
        for (const char of ["⟦", "│", "中", "é", "👩‍💻"]) {
            expect(write({ x: [char.repeat(112)] })[1]).toBe(`x = ["${char.repeat(112)}"]`);
            expect(write({ x: [char.repeat(113)] })[1]).toBe("x = [");
        }
    });

    test("tables and lists in a list take a line each, text of several lines too", () => {
        expect(write({ x: [{ a: 1 }, [2]], y: ["one\ntwo", "three"] })).toEqual([
            "[t]",
            "x = [",
            "    { a = 1 },",
            "    [2],",
            "]",
            "y = [",
            "    '''",
            "one",
            "two",
            "''',",
            '    "three",',
            "]",
            "",
        ]);
    });
});
