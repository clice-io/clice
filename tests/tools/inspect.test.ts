/// Tests for the inspect driver (tools/snap/inspect.ts): the
/// `clice inspect` modes the snap runner itself never takes — CDB
/// discovery, header donors and the default-flags fallback.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { SNAP_DIR } from "@clice/tools/compile-commands";
import { runInspect } from "@clice/tools/snap/inspect";
import { cliceExecutable } from "@clice/tools/session";

// The snap runner always passes explicit --flags; pin the other documented
// inspect modes — a lone file with the default-flags fallback when no
// compile_commands.json exists anywhere above the input.
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

test("inspect --flags replaces CDB discovery", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // The file needs -DNEED=1 which only --flags provides; the CDB next
        // to it would fail the compile, so passing proves --flags won.
        const file = path.join(tmp, "single.cpp");
        fs.writeFileSync(file, "#if !defined(NEED)\n#error missing flag\n#endif\nint x = NEED;\n");
        fs.writeFileSync(
            path.join(tmp, "compile_commands.json"),
            JSON.stringify([
                {
                    directory: tmp,
                    file,
                    arguments: [
                        "clang++",
                        "-std=c++20",
                        "-DNEED=0",
                        "-DWRONG",
                        "-fsyntax-only",
                        file,
                    ],
                },
            ]),
        );
        const { files } = runInspect(cliceExecutable(), "folding_range", file, {
            flags: ["-std=c++20", "-DNEED=1"],
        });
        const entry = files["single.cpp"];
        expect(entry?.error ?? null).toBeNull();
        expect(entry?.diagnostics ?? null).toBeNull();
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("inspect directory is one unit", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        // Serial module build: main.cpp imports a module interface sibling,
        // so its entry only compiles if the PCM was built first. The
        // support file participates through its marker and every file
        // carries a hash for the stripper-twin check.
        fs.writeFileSync(
            path.join(tmp, "mod.cppm"),
            "export module demo;\nexport inline int one() {\n    return 1;\n}\n",
        );
        fs.writeFileSync(path.join(tmp, "lib.h"), "inline int §two() {\n    return 2;\n}\n");
        fs.writeFileSync(
            path.join(tmp, "main.cpp"),
            'import demo;\n#include "lib.h"\nint main() {\n    return one() + two();\n}\n',
        );
        const { files } = runInspect(cliceExecutable(), "folding_range", tmp, {
            flags: ["-std=c++20"],
        });
        expect(files["main.cpp"]?.error ?? null).toBeNull();
        expect(Array.isArray(files["main.cpp"]?.result)).toBe(true);
        // lib.h opts in via its marker; mod.cppm stays a support file.
        expect(Array.isArray(files["lib.h"]?.result)).toBe(true);
        expect(files["mod.cppm"]?.result ?? null).toBeNull();
        expect(files["mod.cppm"]?.stripped_hash).toBeTruthy();
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});
