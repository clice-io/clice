import { describe, expect, test } from "vitest";
import { importsModule } from "./inspect.ts";

describe("importsModule", () => {
    test("a C source's import line is prose", () => {
        const clinic =
            "/*[clinic input]\n    import math.integer\n[clinic start generated code]*/\n";
        expect(importsModule("Modules/m.c", clinic)).toBe(false);
        expect(importsModule("m.cpp", clinic)).toBe(true);
        expect(importsModule("m.cppm", "export import :part;\n")).toBe(true);
    });
});
