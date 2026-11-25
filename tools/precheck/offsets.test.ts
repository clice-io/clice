import { describe, expect, test } from "vitest";
import { unlabelledOffsets } from "./offsets.ts";

const source = (...lines: string[]): string => lines.join("\n");
const unlabelled = (...lines: string[]): string[] =>
    unlabelledOffsets("src/feature/a.cpp", source(...lines));

describe("unlabelledOffsets", () => {
    test("every way to clang's offsets needs a label", () => {
        expect(
            unlabelled(
                "auto [fid, offset] = SM.getDecomposedLoc(location);",
                "auto at = SM.getDecomposedExpansionLoc(location);",
                "auto offset = SM.getFileOffset(location);",
                "auto location = SM.getComposedLoc(fid, 0);",
                "auto start = SM.getLocForStartOfFile(fid);",
                "auto raw = location.getRawEncoding();",
                "auto line = SM.getSpellingLineNumber(location);",
                "auto column = SM.getColumnNumber(fid, offset);",
                "auto place = SM.translateLineCol(fid, 1, 1);",
            ),
        ).toEqual([
            "src/feature/a.cpp:1: auto [fid, offset] = SM.getDecomposedLoc(location);",
            "src/feature/a.cpp:2: auto at = SM.getDecomposedExpansionLoc(location);",
            "src/feature/a.cpp:3: auto offset = SM.getFileOffset(location);",
            "src/feature/a.cpp:4: auto location = SM.getComposedLoc(fid, 0);",
            "src/feature/a.cpp:5: auto start = SM.getLocForStartOfFile(fid);",
            "src/feature/a.cpp:6: auto raw = location.getRawEncoding();",
            "src/feature/a.cpp:7: auto line = SM.getSpellingLineNumber(location);",
            "src/feature/a.cpp:8: auto column = SM.getColumnNumber(fid, offset);",
            "src/feature/a.cpp:9: auto place = SM.translateLineCol(fid, 1, 1);",
        ]);
    });

    test("the label allows the statement below it, the whole statement", () => {
        expect(
            unlabelled(
                "// Clang's offsets: read in the buffer they come from.",
                "auto [fid, offset] =",
                "    SM.getDecomposedLoc(location);",
            ),
        ).toEqual([]);
    });

    test("a label above another statement allows nothing", () => {
        expect(
            unlabelled(
                "// Clang's offsets: read in the buffer they come from.",
                "auto buffer = SM.getBufferData(fid);",
                "auto offset = SM.getFileOffset(location);",
            ),
        ).toEqual(["src/feature/a.cpp:3: auto offset = SM.getFileOffset(location);"]);
    });

    test("the unit and its token map convert, and need none", () => {
        const line = "auto [fid, offset] = SM().getDecomposedLoc(location);";
        expect(unlabelledOffsets("src/compile/compilation_unit.cpp", line)).toEqual([]);
        expect(unlabelledOffsets("src/compile/tokens.cpp", line)).toEqual([]);
    });

    test("the unit's own API passes", () => {
        expect(
            unlabelled(
                "auto [fid, offset] = unit.decompose(location);",
                "auto location = unit.create_location(fid, offset);",
            ),
        ).toEqual([]);
    });
});
