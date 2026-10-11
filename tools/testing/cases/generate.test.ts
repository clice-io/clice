import { expect, test } from "vitest";
import { functions, padded, unitSymbols, units } from "./generate.ts";

test("generated names are numbered", () => {
    expect(Object.keys(units(12)).slice(0, 2)).toEqual(["unit_00.cpp", "unit_01.cpp"]);
    expect(unitSymbols(12).at(-1)).toBe("unit_11");
    expect(units(1)).toEqual({ "unit_0.cpp": "int unit_0() { return 0; }\n" });
    expect(functions(2)).toBe("int function_0() { return 0; }\nint function_1() { return 1; }\n");
    expect(Buffer.byteLength(padded("int x;\n", 1000))).toBeGreaterThanOrEqual(1000);
});
