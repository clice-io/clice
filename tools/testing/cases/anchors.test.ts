import { expect, test } from "vitest";
import { at, locate, spanOf, type MarkerAnchor } from "./anchors.ts";

test("markers fail once edited", () => {
    const text = "int value = 1;\n";
    const marker: MarkerAnchor = {
        path: "a.cpp",
        name: "v",
        begin: 4,
        end: 9,
        range: true,
        text,
    };
    expect(locate(text, marker)).toBe(4);
    expect(() => locate("long value = 1;\n", marker)).toThrow(/changed since/);
    expect(() => spanOf("long value = 1;\n", marker)).toThrow(/changed since/);
    expect(locate("long value = 1;\n", at("a.cpp", "long |value"))).toBe(5);
});
