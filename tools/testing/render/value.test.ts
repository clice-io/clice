import { describe, expect, test } from "vitest";
import { writeToml, type Table } from "./value.ts";

function write(table: Table): string[] {
    return writeToml([{ path: ["t"], table }]).split("\n");
}

const a = (n: number) => "a".repeat(n);

/// Pinned against tombi 1.8.5 formatting TOML 1.1 under tombi.toml's rules:
/// every expected text here is one tombi leaves as it is.
describe("snap.toml is laid out as tombi formats it", () => {
    test("a list stays on a line of 120 columns, a longer one takes a line per item", () => {
        expect(write({ x: [a(112)] })).toEqual(["[t]", `x = ["${a(112)}"]`, ""]);
        expect(write({ x: [a(113)] })).toEqual(["[t]", "x = [", `    "${a(113)}",`, "]", ""]);
    });

    test("a column is a grapheme", () => {
        for (const char of ["⟦", "│", "中", "é", "👩‍💻"]) {
            expect(write({ x: [char.repeat(112)] })[1]).toBe(`x = ["${char.repeat(112)}"]`);
            expect(write({ x: [char.repeat(113)] })[1]).toBe("x = [");
        }
    });

    test("a list's row counts its indentation once an item", () => {
        expect(write({ x: [{ a: a(93), b: "b" }] })).toEqual([
            "[t]",
            "x = [",
            `    { a = "${a(93)}", b = "b" },`,
            "]",
            "",
        ]);
        expect(write({ x: [{ a: a(94), b: "b" }] })).toEqual([
            "[t]",
            "x = [",
            "    {",
            `        a = "${a(94)}",`,
            '        b = "b",',
            "    },",
            "]",
            "",
        ]);
    });

    test("and so does every list or table in the row", () => {
        const digits = (n: number) => Array.from({ length: n }, (_, i) => i % 10);
        expect(write({ x: [{ n: digits(17) }] })[2]).toBe(
            `    { n = [${digits(17).join(", ")}] },`,
        );
        expect(write({ x: [{ n: digits(18) }] })).toEqual([
            "[t]",
            "x = [",
            "    {",
            `        n = [${digits(18).join(", ")}],`,
            "    },",
            "]",
            "",
        ]);
    });

    test("a field's table is measured from its column", () => {
        const row = (n: number) => write({ x: [{ long: a(120), r: { a: a(n) } }] })[4];
        expect(row(98)).toBe(`        r = { a = "${a(98)}" },`);
        expect(row(99)).toBe("        r = {");
    });

    test("an empty table ending a list counts as tombi measures it, `{  }`", () => {
        expect(write({ x: [a(107), {}] })[1]).toBe(`x = ["${a(107)}", {}]`);
        expect(write({ x: [a(108), {}] })).toEqual([
            "[t]",
            "x = [",
            `    "${a(108)}",`,
            "    {},",
            "]",
            "",
        ]);
    });

    test("an item holding only tables has its header right above the first", () => {
        expect(write({ l: [{ s: { t: "one\ntwo" } }, { k: 1 }] })).toEqual([
            "[[t.l]]",
            "[t.l.s]",
            "t = '''",
            "one",
            "two",
            "'''",
            "",
            "[[t.l]]",
            "k = 1",
            "",
        ]);
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
