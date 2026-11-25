import { describe, expect, test } from "vitest";
import type { Option } from "./options.ts";
import { changes, parseMacros, render, searchList } from "./compilers.ts";

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

    test("a change the tool accepts is reported apart, with its reason", () => {
        const option = (id: string, spelling: string): Option =>
            ({ id, spelling }) as unknown as Option;
        const report = render(
            ["# report"],
            [
                {
                    option: option("_SLASH_Zc___cplusplus", "/Zc:__cplusplus"),
                    compiler: "cl.exe",
                    language: "c++",
                    variant: ["/Zc:__cplusplus"],
                    changes: ["+__cplusplus=201402L"],
                },
                {
                    option: option("mthreads", "-mthreads"),
                    compiler: "g++",
                    language: "c++",
                    variant: ["-mthreads"],
                    changes: ["+_MT=1"],
                },
            ],
        );
        expect(report.split("\n")).toEqual([
            "# report",
            "",
            "## Options the table must keep",
            "",
            "- `-mthreads` mthreads",
            "  - `-mthreads` (g++ c++): +_MT=1",
            "",
            "## Accepted",
            "",
            "- `/Zc:__cplusplus` _SLASH_Zc___cplusplus: clang-cl's __cplusplus follows the standard whatever /Zc:__cplusplus says, and a cl command gets clang-cl's",
            "  - `/Zc:__cplusplus` (cl.exe c++): +__cplusplus=201402L",
            "",
        ]);
    });
});
