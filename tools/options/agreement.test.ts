import { describe, expect, test } from "vitest";
import { accepted, checks } from "./agreement.ts";

describe("checks", () => {
    test("every macro is defined, an integer one with its value, none the option removes", () => {
        const source = checks(
            {
                cc1: [],
                macros: new Set(["#define A 1", "#define F(x) x", '#define S "s"']),
                features: new Set(["__has_feature(x)"]),
            },
            {
                cc1: [],
                macros: new Set(["#define A 1", "#define B 2"]),
                features: new Set(["__has_feature(x)", "__has_feature(y)"]),
            },
        );
        expect(source.split("\n")).toEqual([
            "#ifndef A",
            '#error "missing A"',
            "#endif",
            "#if (A) != (1)",
            '#error "value A"',
            "#endif",
            "#ifndef F",
            '#error "missing F"',
            "#endif",
            "#ifndef S",
            '#error "missing S"',
            "#endif",
            "#ifdef B",
            '#error "extra B"',
            "#endif",
            "#if !__has_feature(x)",
            '#error "missing __has_feature(x)"',
            "#endif",
            "#if __has_feature(y)",
            '#error "extra __has_feature(y)"',
            "#endif",
            "",
        ]);
    });
});

describe("accepted", () => {
    test("an entry covers its option on its target, or on every target", () => {
        expect(accepted("fmodules", "x86")).toBeDefined();
        expect(accepted("fno_ms_extensions", "cl")).toBeDefined();
        expect(accepted("fno_ms_extensions", "x86")).toBeUndefined();
        expect(accepted("fPIC", "x86")).toBeUndefined();
    });
});
