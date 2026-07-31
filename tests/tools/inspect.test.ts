/// Tests for the inspect driver helpers (tools/snap/inspect.ts):
/// fixture metadata, the raw renderers, and the `clice inspect` modes the
/// snap runner itself never takes.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { SNAP_DIR } from "@clice/tools/compile-commands";
import { parseAnnotations } from "@clice/tools/snap/annotation";
import {
    focusSemanticTokens,
    parseFixtureMeta,
    rawSemanticTokenPieces,
    renderRawSemanticTokens,
    runInspect,
    semanticTokenFocusOffsets,
} from "@clice/tools/snap/inspect";
import { decodeSemanticTokenPieces, decodeSemanticTokens } from "@clice/tools/snap/presenters";
import { cliceExecutable } from "@clice/tools/session";

test("fixture meta parsing", () => {
    const header = "/// # Title\n///\n/// - status: partial\n/// - snap: separate\nint x;\n";
    expect(parseFixtureMeta(header, "f")).toEqual({ status: "partial", snap: "separate" });
    expect(parseFixtureMeta("/// # T\n///\n/// - snap: skip\n", "f").snap).toBe("skip");
    // Bulleted lines in the markdown description after the blank `///`
    // separator are prose, not metadata.
    const withDesc =
        "/// # T\n///\n/// ## I\n///\n/// - status: partial\n///\n/// - lorem: prose\nint x;\n";
    expect(parseFixtureMeta(withDesc, "f")).toEqual({ status: "partial", snap: "shared" });
    // No header: defaults, and shared is the default mode.
    expect(parseFixtureMeta("int x;\n", "f")).toEqual({ status: "supported", snap: "shared" });
    expect(() => parseFixtureMeta("/// # T\n///\n/// - snpa: separate\n", "f")).toThrow(
        "unknown fixture meta key",
    );
    // Near-miss spellings must error too, not silently end the block.
    expect(() => parseFixtureMeta("/// # T\n///\n/// - Snap: shared\n", "f")).toThrow(
        "unknown fixture meta key",
    );
    expect(() => parseFixtureMeta("/// # T\n///\n/// - snap: shred\n", "f")).toThrow(
        "invalid snap mode",
    );
    // A repeated key must not silently let the later value win.
    expect(() =>
        parseFixtureMeta("/// # T\n///\n/// - snap: shared\n/// - snap: skip\n", "f"),
    ).toThrow("duplicate fixture meta key");
});

// The snap runner always passes single files with a CDB next to them; pin
// the other documented inspect modes — a lone file with the default-flags
// fallback when no compile_commands.json exists anywhere above the input.
test("inspect single file without CDB", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        const file = path.join(tmp, "single.cpp");
        fs.copyFileSync(path.join(SNAP_DIR, "folding_range", "block_folding.cpp"), file);
        const { files } = runInspect(cliceExecutable(), "folding_range", file);
        const entry = files["single.cpp"];
        expect(entry?.error ?? null).toBeNull();
        const result = entry?.result;
        expect(Array.isArray(result) && result.length > 0).toBe(true);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("inspect treats bare headers as C++ in the fallback", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // Namespaces are C++-only: an ambiguous .h must default to C++
        // (clangd convention), not the C driver its extension suggests.
        const file = path.join(tmp, "single.h");
        fs.writeFileSync(file, "namespace demo {\ninline int one() {\n    return 1;\n}\n}\n");
        const { files } = runInspect(cliceExecutable(), "folding_range", file);
        expect(files["single.h"]?.error ?? null).toBeNull();
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("inspect headers borrow the nearest TU command", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // The header only compiles with the TU's -D flag, so passing means
        // the CDB-less header inherited the donor command instead of the
        // generic fallback.
        fs.writeFileSync(path.join(tmp, "main.cpp"), '#include "lib.h"\n');
        fs.writeFileSync(
            path.join(tmp, "lib.h"),
            "#if !defined(NEED)\n#error missing project define\n#endif\nnamespace demo {\ninline int one() {\n    return NEED;\n}\n}\n",
        );
        fs.writeFileSync(
            path.join(tmp, "compile_commands.json"),
            JSON.stringify([
                {
                    directory: tmp,
                    file: path.join(tmp, "main.cpp"),
                    arguments: [
                        "clang++",
                        "-std=c++20",
                        "-DNEED=1",
                        "-fsyntax-only",
                        path.join(tmp, "main.cpp"),
                    ],
                },
            ]),
        );
        const { files } = runInspect(cliceExecutable(), "folding_range", path.join(tmp, "lib.h"));
        expect(files["lib.h"]?.error ?? null).toBeNull();
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("inspect header donors are language compatible", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // The C entry comes first in the database; a C++ header must still
        // pick the C++ TU as its donor (with that TU's define).
        fs.writeFileSync(path.join(tmp, "main.c"), "int main(void) { return 0; }\n");
        fs.writeFileSync(path.join(tmp, "app.cpp"), '#include "lib.hpp"\n');
        fs.writeFileSync(
            path.join(tmp, "lib.hpp"),
            "#if !defined(NEED)\n#error missing project define\n#endif\nnamespace demo {\ninline int one() {\n    return NEED;\n}\n}\n",
        );
        const entry = (file: string, driver: string, extra: string[]) => ({
            directory: tmp,
            file: path.join(tmp, file),
            arguments: [driver, ...extra, "-fsyntax-only", path.join(tmp, file)],
        });
        fs.writeFileSync(
            path.join(tmp, "compile_commands.json"),
            JSON.stringify([
                entry("main.c", "clang", []),
                entry("app.cpp", "clang++", ["-std=c++20", "-DNEED=1"]),
            ]),
        );
        const { files } = runInspect(cliceExecutable(), "folding_range", path.join(tmp, "lib.hpp"));
        expect(files["lib.hpp"]?.error ?? null).toBeNull();
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("inspect keeps C sources C in the fallback", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // _Generic is C-only: this compiles iff the fallback picks a C
        // driver instead of forcing clang++ onto every extension.
        const file = path.join(tmp, "single.c");
        fs.writeFileSync(
            file,
            "int pick(int x) {\n    return _Generic(x, int: 1, default: 0);\n}\n",
        );
        const { files } = runInspect(cliceExecutable(), "folding_range", file);
        expect(files["single.c"]?.error ?? null).toBeNull();
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("inspect surfaces errors on completed compiles", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // A type name used as an expression: the AST still builds (no
        // `error` in the entry), but the error diagnostics must surface —
        // the snap harness's does-not-compile-cleanly gate depends on it.
        const file = path.join(tmp, "broken.cpp");
        fs.writeFileSync(file, "struct W { W(int); W make() { return W; } };\n");
        const { files } = runInspect(cliceExecutable(), "semantic_tokens", file);
        const entry = files["broken.cpp"];
        expect(entry?.error ?? null).toBeNull();
        expect((entry?.diagnostics ?? []).length).toBeGreaterThan(0);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("marker focus filters tokens and pins misses", () => {
    // A named and a nameless marker on the same offset collapse to one
    // entry; the marker on `=` (no token there) must render an explicit
    // `kind: none` line so the absence itself is snapshot content.
    const source = parseAnnotations("int §(v)§a §= done;\n");
    const stripped = Buffer.from(source.content);
    const raw = [
        { range: { begin: 0, end: 3 }, kind: "Keyword", modifiers: 0 },
        { range: { begin: 4, end: 5 }, kind: "Variable", modifiers: 0 },
    ];
    const offsets = semanticTokenFocusOffsets(source);
    expect(offsets).toEqual([4, 6]);
    const focused = focusSemanticTokens(rawSemanticTokenPieces(raw, stripped), offsets, stripped);
    expect(focused).toEqual([
        '- { loc: "0:4", text: "a", kind: variable }',
        '- { loc: "0:6", text: "=", kind: none }',
    ]);

    // A marker on an identifier no token covers keeps the identifier as
    // the miss label, so a later fix flips kind while the text stays put.
    const bare = parseAnnotations("goto §done;\n");
    const none = focusSemanticTokens(
        [],
        semanticTokenFocusOffsets(bare),
        Buffer.from(bare.content),
    );
    expect(none).toEqual(['- { loc: "0:5", text: "done", kind: none }']);

    // A miss on a non-ASCII position labels with the whole UTF-8 sequence,
    // not a lone byte turned into mojibake.
    const cjk = parseAnnotations("int x; // §你好\n");
    const cjkOffsets = semanticTokenFocusOffsets(cjk);
    expect(focusSemanticTokens([], cjkOffsets, Buffer.from(cjk.content))).toEqual([
        '- { loc: "0:10", text: "你", kind: none }',
    ]);

    // Range markers have no focus meaning; silently pinning the full dump
    // would hide the author's intent, so they are rejected.
    expect(() => semanticTokenFocusOffsets(parseAnnotations("§⟦int⟧ x;\n"))).toThrow(
        "point markers",
    );
});

test("marker focus agrees between standalone and wire pieces", () => {
    const source = parseAnnotations("int §a = §1;\n");
    const stripped = Buffer.from(source.content);
    const raw = [
        { range: { begin: 0, end: 3 }, kind: "Keyword", modifiers: 0 },
        { range: { begin: 4, end: 5 }, kind: "Variable", modifiers: 0 },
        { range: { begin: 8, end: 9 }, kind: "Number", modifiers: 0 },
    ];
    const legend = { tokenTypes: ["keyword", "variable", "number"], tokenModifiers: [] };
    const wire = decodeSemanticTokenPieces(
        [0, 0, 3, 0, 0, 0, 4, 1, 1, 0, 0, 4, 1, 2, 0],
        source.content.split("\n"),
        legend,
    );
    const offsets = semanticTokenFocusOffsets(source);
    const standalone = focusSemanticTokens(
        rawSemanticTokenPieces(raw, stripped),
        offsets,
        stripped,
    );
    expect(standalone).toEqual(focusSemanticTokens(wire, offsets, stripped));
    expect(standalone).toEqual([
        '- { loc: "0:4", text: "a", kind: variable }',
        '- { loc: "0:8", text: "1", kind: number }',
    ]);
});

test("multiline token split matches wire decode", () => {
    // A token spanning a blank line: the server splits it per line and the
    // blank interior piece still encodes (its newline counts), so both
    // renderers must emit the empty-text entry identically.
    const content = "/*x\n\ny*/\nint a;\n";
    const raw = [{ range: { begin: 0, end: 8 }, kind: "Comment", modifiers: 0 }];
    const standalone = renderRawSemanticTokens(raw, Buffer.from(content));
    const legend = { tokenTypes: ["comment"], tokenModifiers: [] };
    const wire = decodeSemanticTokens(
        [0, 0, 4, 0, 0, 1, 0, 1, 0, 0, 1, 0, 3, 0, 0],
        content.split("\n"),
        legend,
    );
    expect(standalone).toEqual(wire);
    expect(standalone[1]).toBe('- { loc: "1:0", text: "", kind: comment }');
});
