import { describe, expect, test } from "vitest";
import { DEFAULTS, hex, parseInteger, splitLines, symbolize, type Resolve } from "./symbolize.ts";

/// A resolver answering every offset with one function named after it, and
/// the offsets it was asked for.
function recording(): { resolve: Resolve; asked: bigint[] } {
    const asked: bigint[] = [];
    return {
        asked,
        resolve: (offset) => {
            asked.push(offset);
            return [`${hex(offset)}: f${offset.toString(16)} + 1 @ a.cpp:1`];
        },
    };
}

function run(lines: readonly string[], options = DEFAULTS, resolve?: Resolve) {
    const recorder = recording();
    const result = symbolize(`${lines.join("\n")}\n`, options, resolve ?? recorder.resolve);
    if ("error" in result) {
        throw new Error(result.error);
    }
    return { lines: result.lines, asked: recorder.asked };
}

describe("splitLines", () => {
    test("splits at every line break, the last one ending the last line", () => {
        expect(splitLines("a\r\nb\rc\nd\fe\u2028f\n")).toEqual(["a", "b", "c", "d", "e", "f"]);
        expect(splitLines("a\n\n")).toEqual(["a", ""]);
        expect(splitLines("a")).toEqual(["a"]);
        expect(splitLines("")).toEqual([]);
    });
});

describe("parseInteger", () => {
    test("reads decimal and prefixed literals, with underscores and a sign", () => {
        expect(parseInteger("0x140000000")).toBe(0x140000000n);
        expect(parseInteger(" 0X1_4000_0000 ")).toBe(0x140000000n);
        expect(parseInteger("5368709120")).toBe(0x140000000n);
        expect(parseInteger("0o17")).toBe(15n);
        expect(parseInteger("0b101")).toBe(5n);
        expect(parseInteger("-0x10")).toBe(-16n);
        expect(parseInteger("0")).toBe(0n);
        expect(parseInteger("00")).toBe(0n);
    });

    test("rejects what is no literal", () => {
        for (const text of ["", "0x", "010", "1__0", "_1", "1_", "0x12z", "abc", "0x-1"]) {
            expect(parseInteger(text), text).toBeUndefined();
        }
    });
});

describe("symbolize", () => {
    const header = [
        "[2026-11-22 19:00:58.745] [fatal] clice crashed",
        "=== CRASH STACK TRACE ===",
        "clice 1.0.0+g92d26ad x86_64-unknown-linux-gnu https://github.com/clice-io/clice/tree/92d26ad688e439961d6176cf3d30270bd898c55e",
    ];

    test("a Linux frame's offset is its address, the byte before for a return address", () => {
        const { lines, asked } = run([
            ...header,
            "0x00005FF1AC363B08, /tmp/pkg/clice/bin/clice(0x00005FF1A864A000) + 0x3D19B08 byte(s) (interrupted)",
            "0x00005FF1A951A640, /tmp/pkg/clice/bin/clice(0x00005FF1A864A000) + 0xED0640 byte(s)",
            "0x000071DE25D99CA8, /lib/x86_64-linux-gnu/libc.so.6(0x000071DE25D70000) + 0x29CA8 byte(s)",
            "0x00005FF1AA23543A, /tmp/pkg/clice/bin/clice(0x00005FF1A864A000) + 0x1BEB43A byte(s)",
            "0x00007FFD12345678 <unknown module>",
        ]);
        expect(asked).toEqual([0x3d19b08n, 0xed063fn, 0x1beb439n]);
        expect(lines).toEqual([
            ...header,
            "#0 0x3d19b08 0x3d19b08: f3d19b08 + 1 @ a.cpp:1",
            "#1 0xed063f 0xed063f: fed063f + 1 @ a.cpp:1",
            "0x000071DE25D99CA8, /lib/x86_64-linux-gnu/libc.so.6(0x000071DE25D70000) + 0x29CA8 byte(s)",
            "#3 0x1beb439 0x1beb439: f1beb439 + 1 @ a.cpp:1",
            "0x00007FFD12345678 <unknown module>",
        ]);
    });

    test("a macOS frame's offset is its address, with no image base", () => {
        const { asked } = run([
            "0x000000010A2B3C4D, /Applications/clice/bin/clice(0x0000000106000000) + 0x1002B3C4D byte(s) (interrupted)",
            "0x00007FF8012A3B4C, /usr/lib/system/libsystem_c.dylib(0x00007FF801200000) + 0xA3B4C byte(s)",
        ]);
        expect(asked).toEqual([0x1002b3c4dn]);
    });

    test("a Windows module's offset counts from the preferred image base", () => {
        const { lines, asked } = run([
            "0x00007FF6A1B2C3D4, C:\\Program Files (x86)\\clice\\bin\\CLICE.EXE(0x00007FF6A1B20000) + 0x2C3D4 byte(s) (interrupted)",
            "0x00007FFB1C2D3E4F, C:\\Windows\\System32\\KERNEL32.DLL(0x00007FFB1C2C0000) + 0x13E4F byte(s)",
            "0x00007FF6A1B2D000, C:/clice/bin/clice.exe(0x00007FF6A1B20000) + 0x2D000 byte(s)",
        ]);
        expect(asked).toEqual([0x14002c3d4n, 0x14002cfffn]);
        expect(lines[2]).toBe("#2 0x14002cfff 0x14002cfff: f14002cfff + 1 @ a.cpp:1");
    });

    test("a DLL of the main executable's name and another image base", () => {
        const { asked } = run(
            ["0x00007FF6A1B2C3D4, D:\\clice\\clice.dll(0x00007FF6A1B20000) + 0x100 byte(s)"],
            { ...DEFAULTS, imageBase: 0x180000000n },
        );
        expect(asked).toEqual([0x1800000ffn]);
    });

    test("an older Windows log, LLVM's trace, marks no frame interrupted", () => {
        const { asked } = run([
            "Stack dump:",
            "0x00007FF6A1B2C3D4, C:\\clice\\bin\\clice.exe(0x00007FF6A1B20000) + 0x2C3D4 byte(s)",
            "0x00007FF6A1B2C3D4, C:\\clice\\bin\\clice.exe(0x00007FF6A1B20000) + 0x2C3E0 byte(s), clang::Lexer::Lex() + 0x20 bytes(s)",
        ]);
        expect(asked).toEqual([0x14002c3d3n, 0x14002c3dfn]);
    });

    test("module frames are numbered per crash section", () => {
        const frame =
            "0x00005FF1A951A640, /usr/bin/clice(0x00005FF1A864A000) + 0x10 byte(s) (interrupted)";
        const { lines } = run([frame, frame, "=== CRASH STACK TRACE ===", frame]);
        expect(lines.map((line) => line.split(" ")[0])).toEqual(["#0", "#1", "===", "#0"]);
    });

    test("an older log's numbered frames are rebased by the latest base line", () => {
        const { lines, asked } = run([
            "main executable base: 0x5ff1a864a000",
            "Stack dump without symbol names (ensure you have llvm-symbolizer in your PATH):",
            "0  clice 0x00005ff1ac363b08",
            "1  /opt/clice/bin/clice 0x00005ff1a951a640",
            "2  libc.so.6 0x000071de25d99ca8",
            "3  clice-other 0x00005ff1aa23543a",
            "4  clice 0x0000000000001000",
            "#0 0x0000000000abcdef in already symbolized",
            "main executable base: 0x561200000000",
            "0 clice 0x561200000010",
        ]);
        expect(asked).toEqual([0x3d19b08n, 0xed0640n, 0x10n]);
        expect(lines).toEqual([
            "main executable base: 0x5ff1a864a000",
            "Stack dump without symbol names (ensure you have llvm-symbolizer in your PATH):",
            "#0 0x3d19b08 0x3d19b08: f3d19b08 + 1 @ a.cpp:1",
            "#1 0xed0640 0xed0640: fed0640 + 1 @ a.cpp:1",
            "2  libc.so.6 0x000071de25d99ca8",
            "3  clice-other 0x00005ff1aa23543a",
            "4  clice 0x0000000000001000",
            "#0 0x0000000000abcdef in already symbolized",
            "main executable base: 0x561200000000",
            "#0 0x10 0x10: f10 + 1 @ a.cpp:1",
        ]);
    });

    test("an older log's frames name the module case-sensitively", () => {
        const { asked } = run(["main executable base: 0x1000", "0  Clice 0x2000"]);
        expect(asked).toEqual([]);
    });

    test("--module picks the main executable; another's frames pass through", () => {
        const frame = "0x0000000000002000, /usr/bin/clice(0x1000) + 0x1000 byte(s)";
        expect(run([frame], { ...DEFAULTS, module: "other" }).asked).toEqual([]);
        expect(run([frame], { ...DEFAULTS, module: "CLICE" }).asked).toEqual([0xfffn]);
    });

    test("inlined functions go on lines under the frame; unresolved frames stay", () => {
        const frames = [
            "0x0000000000002000, /usr/bin/clice(0x1000) + 0x1000 byte(s) (interrupted)",
            "0x0000000000003000, /usr/bin/clice(0x1000) + 0x2000 byte(s)",
        ];
        const { lines } = run(frames, DEFAULTS, (offset) =>
            offset === 0x1000n
                ? ["0x1000: HandleDirective + 3 @ a.cpp:1 [inlined]", "    Lex + 7 @ b.cpp:2"]
                : undefined,
        );
        expect(lines).toEqual([
            "#0 0x1000 0x1000: HandleDirective + 3 @ a.cpp:1 [inlined]\n        Lex + 7 @ b.cpp:2",
            frames[1],
        ]);
    });

    test("a log with nothing to rebase by is an error", () => {
        const result = symbolize(
            "[info] no crash\n0  clice 0x00005ff1ac363b08\n#0 0x1234 in main\n",
            DEFAULTS,
            recording().resolve,
        );
        expect(result).toEqual({
            error:
                "no frame naming its module and no 'main executable base' line in the log; " +
                "the crash predates base recording — addresses cannot be rebased",
        });
    });
});
