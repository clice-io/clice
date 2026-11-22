import { describe, expect, test } from "vitest";
import { failures, symbolizedFrames } from "./crash_symbols.ts";

/// The v1.0.0 Linux package's crash as symbolize.ts prints it, its paths
/// shortened.
const LINUX = `
[2026-11-22 18:54:43.811] [fatal] clice crashed
=== CRASH STACK TRACE ===
clice 1.0.0+g92d26ad x86_64-unknown-linux-gnu https://github.com/clice-io/clice/tree/92d26ad688e439961d6176cf3d30270bd898c55e
#0 0x3d19b08 0x0000000003d19b08: _ZN12_GLOBAL__N_118PragmaDebugHandler12HandlePragmaERN5clang12PreprocessorENS1_16PragmaIntroducerERNS1_5TokenE + 600 @ clang/lib/Lex/Pragma.cpp:0
#1 0xed063f 0x0000000000ed063f: _ZN5clang12Preprocessor21HandlePragmaDirectiveENS_16PragmaIntroducerE + 79 @ clang/lib/Lex/Pragma.cpp:175
#2 0xeb93d4 0x0000000000eb93d4: HandleDirective + 39319 @ clang/lib/Lex/PPDirectives.cpp:1454 [inlined]
                        LexTokenInternal + 41831 @ clang/lib/Lex/Lexer.cpp:4630 [inlined]
                        _ZN5clang5Lexer3LexERNS_5TokenE + 41940 @ clang/lib/Lex/Lexer.cpp:3825
#6 0x21b79b8 0x00000000021b79b8: _ZN5cliceW5clice15CompilationUnit3runEv + 2936 @ ./src/compile/compilation.cpp:1039
#9 0x1bebe89 0x0000000001bebe89: _ZNK7kotatsu3cli8CommandsIN5clice6driverW5clice11SubcommandsEE6invokeIiJEJEEET_PFS8_DpT0_EDpOT1_ + 22 @ kotatsu/cli/commands.h:137 [inlined]
                        main + 2489 @ ./src/clice.cc:78
0x0000756024BE1CA8, /lib/x86_64-linux-gnu/libc.so.6(0x0000756024BB8000) + 0x29CA8 byte(s)
#12 0x1beb439 0x0000000001beb439: _start + 40
`;

describe("symbolizedFrames", () => {
    test("each frame's function and the ones inlined into it", () => {
        const frames = symbolizedFrames(LINUX);
        // _start's frame names no source location.
        expect([...frames.keys()]).toEqual([0, 1, 2, 6, 9]);
        expect(frames.get(2)).toEqual([
            "HandleDirective",
            "LexTokenInternal",
            "_ZN5clang5Lexer3LexERNS_5TokenE",
        ]);
        expect(frames.get(9)?.at(-1)).toBe("main");
    });

    test("an indented line after anything but a frame belongs to none", () => {
        const frames = symbolizedFrames(
            [
                "#0 0x10 0x0000000000000010: f + 1 @ a.cpp:1",
                "0x0000756024BE1CA8, /lib/x86_64-linux-gnu/libc.so.6(0x0000756024BB8000) + 0x29CA8 byte(s)",
                "    g + 2 @ b.cpp:2 [inlined]",
            ].join("\n"),
        );
        expect([...frames]).toEqual([[0, ["f"]]]);
    });
});

describe("failures", () => {
    test("the Linux crash names HandlePragma, a clice function and main", () => {
        expect(failures(symbolizedFrames(LINUX))).toEqual([]);
    });

    test("a Windows crash names clang's anonymous-namespace functions bare", () => {
        const windows = [
            "#0 0x1401b94a0 0x00000001401b94a0: HandlePragma + 32 @ clang/lib/Lex/Pragma.cpp:1098",
            "#1 0x1421b79b8 0x00000001421b79b8: _ZN5cliceW5clice15CompilationUnit3runEv + 2936 @ ./src/compile/compilation.cpp:1039",
            "#2 0x141bebe89 0x0000000141bebe89: main + 2489 @ ./src/clice.cc:78",
        ].join("\n");
        expect(failures(symbolizedFrames(windows))).toEqual([]);
    });

    test("each function the crash lacks is named", () => {
        const unresolved = [
            "#0 0x3d19b08 0x0000000003d19b08: _ZN5clang12Preprocessor3LexERNS_5TokenE + 35 @ a.cpp:1",
            "    main + 1 @ b.cpp:2 [inlined]",
            "#1 0xed063f 0x0000000000ed063f: _start + 40",
        ].join("\n");
        expect(failures(symbolizedFrames(unresolved))).toEqual([
            "the interrupted frame is not clang's HandlePragma",
            "no frame under it is clice's",
            "no frame under it is main",
        ]);
        expect(failures(new Map())).toHaveLength(3);
    });
});
