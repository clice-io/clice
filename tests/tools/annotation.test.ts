/// Tests for the TS annotation parser (tools/snap/annotation.ts), the
/// byte-identical twin of src/syntax/annotation.cpp.

import { expect, test } from "vitest";
import { parseAnnotations } from "@clice/tools/snap/annotation";

test("annotation passthrough", () => {
    const src = parseAnnotations("int x = 1;\n");
    expect(src.content).toBe("int x = 1;\n");
    expect(src.offsets.size).toBe(0);
    expect(src.ranges.size).toBe(0);
    expect(src.namelessOffsets).toEqual([]);
});

test("annotation points and ranges", () => {
    const src = parseAnnotations("int §(a)x = §1;\n§(r)⟦int §⟦y⟧;⟧\n");
    expect(src.content).toBe("int x = 1;\nint y;\n");
    expect(src.offsets).toEqual(new Map([["a", 4]]));
    expect(src.namelessOffsets).toEqual([8]);
    expect(src.ranges).toEqual(
        new Map([
            ["r", [11, 17]],
            ["", [15, 16]],
        ]),
    );
});

test("annotation byte offsets", () => {
    // Offsets count UTF-8 bytes, matching the C++ side.
    const src = parseAnnotations("/*中*/§(p)x");
    expect(src.content).toBe("/*中*/x");
    expect(src.offsets).toEqual(new Map([["p", 7]]));
});

test("annotation nameless parens", () => {
    const src = parseAnnotations("f§()(1)");
    expect(src.content).toBe("f(1)");
    expect(src.namelessOffsets).toEqual([1]);
});

test.for([
    "§(unterminated",
    "§(not an identifier)",
    "§(café)",
    "§(dup)x §(dup)y",
    "no open⟧",
    "§(d)⟦x⟧ §(d)⟦y⟧",
    "bare ⟦",
    "§⟦unclosed",
    "§(nameless_0)x", // collides with generated keys for unnamed markers
])("annotation rejects %s", (text) => {
    expect(() => parseAnnotations(text)).toThrow();
});
