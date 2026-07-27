/// Tests for the inspect driver helpers (tools/snap/inspect.ts):
/// fixture metadata, the raw renderers, and the `clice inspect` modes the
/// snap runner itself never takes.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { SNAP_DIR } from "@clice/tools/compile-commands";
import { parseFixtureMeta, renderRawSemanticTokens, runInspect } from "@clice/tools/snap/inspect";
import { decodeSemanticTokens } from "@clice/tools/snap/presenters";
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
