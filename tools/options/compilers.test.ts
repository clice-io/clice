import { describe, expect, test } from "vitest";
import { changes, parseMacros, searchList } from "./compilers.ts";

describe("answers", () => {
    test("a dump's macros by name, with or without a value", () => {
        expect(parseMacros("#define _MT 1\r\n#define __ELF__\nint x;\n")).toEqual(
            new Map([
                ["_MT", "1"],
                ["__ELF__", ""],
            ]),
        );
    });

    test("the include search is what -v lists between its markers", () => {
        const stderr = [
            "ignoring nonexistent directory",
            '#include "..." search starts here:',
            " /work/quoted",
            "#include <...> search starts here:",
            " /usr/include",
            "End of search list.",
        ].join("\n");
        expect(searchList(stderr)).toEqual(["/work/quoted", "/usr/include"]);
        expect(searchList("no list")).toEqual([]);
    });

    test("an answer's changes name the macros and directories it adds and removes", () => {
        const before = {
            macros: new Map([
                ["A", "1"],
                ["B", "1"],
            ]),
            search: ["/a"],
        };
        const now = {
            macros: new Map([
                ["A", "2"],
                ["C", ""],
            ]),
            search: ["/b"],
        };
        expect(changes(before, now)).toEqual(["+A=2", "+C=", "-B", "+/b", "-/a"]);
    });
});
