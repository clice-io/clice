/// `clice analyze modules --view interface` and `clice modulize`: a
/// partition's libraries as module interface units over their headers, the
/// standard library as libc++'s std.compat, and the C library kept headers.
/// The libraries, the standard library and its module sources are stand-ins
/// in the workspace, so no real system header is involved.

import { spawnSync } from "node:child_process";
import { statSync } from "node:fs";
import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { type Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test, type SessionFactory } from "../fixtures.ts";

interface Header {
    file: string;
    name: string;
    include: string;
    because: string;
}

interface Interface {
    module: string;
    imports: string[];
    entries: Header[];
    exports: { name: string; file: string; used: boolean }[];
    aliases: { name: string; target: string }[];
    textual: Header[];
    macros: { name: string; module: string; file: string; directive: string }[];
    reads: { name: string }[];
}

interface Plan {
    stdSources: string[];
    modules: {
        name: string;
        source: string;
        imports: string[];
        includeRoots: string[];
        mirrors: string[];
    }[];
    mirrors: string[];
    prelude: string;
    warnings: string[];
}

const SCOPE = "app/**,third/**";

function lines(...text: string[]): string {
    return [...text, ""].join("\n");
}

function runClice(...args: string[]) {
    return spawnSync(cliceExecutable(), args, {
        encoding: "utf8",
        timeout: 120_000,
        maxBuffer: 64 * 1024 * 1024,
    });
}

/// A program over two libraries, beta including alpha, a standard library
/// whose C++ headers include the C library's, and libc++'s module sources
/// for it: std.cppm includes the headers `import std` stands for and
/// std.compat exports fake_puts.
function writeProject(session: SessionFactory): Workspace {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(
        "third/std/fakecstdio",
        lines(
            "#pragma once",
            "#include <cva.h>",
            "#include <cio.h>",
            "#include <cputs.h>",
            "namespace fakestd { inline int fake_abs(int x) { return x < 0 ? -x : x; } }",
        ),
    );
    ws.write("third/std/fakevector", lines("#pragma once", "namespace fakestd { struct vec {}; }"));
    ws.write(
        "third/libc/cio.h",
        lines(
            "#pragma once",
            "#define FAKE_EOF (-1)",
            "extern int fake_stdout;",
            "#define fake_stdout fake_stdout",
            "#define CVA_TYPE_ONLY",
            "#include <cva.h>",
            "#undef CVA_TYPE_ONLY",
        ),
    );
    // Read differently by its includer, as <stdarg.h> is under <stdio.h>'s
    // __need___va_list: <cio.h> takes only its type.
    ws.write(
        "third/libc/cva.h",
        lines(
            "#ifndef CVA_TYPE_ONLY",
            "int fake_vlen(void);",
            "#include <cvp.h>",
            "#endif",
            "typedef int fake_va;",
        ),
    );
    ws.write("third/libc/cvp.h", lines("#pragma once", "int fake_vprint(int);"));
    ws.write("third/libc/cputs.h", lines("#pragma once", "int fake_puts(const char*);"));
    // Included again, it undefines its macro ahead of defining it anew.
    ws.write("third/libc/fassert.h", lines("#undef fassert", "#define fassert(x) ((void)(x))"));
    ws.write(
        "third/alpha/alpha/alpha.h",
        lines(
            "#pragma once",
            "#include <fakecstdio>",
            "#include <fassert.h>",
            "#define ALPHA_VERSION 3",
            "#define ALPHA_TWICE(x) ((x) * 2)",
            "#ifdef ALPHA_WIDE",
            "#endif",
            "#ifdef ALPHA_TMP",
            "#endif",
            "namespace alpha {",
            "struct Thing {",
            "    int v;",
            "    friend bool operator==(Thing, Thing) { return true; }",
            "};",
            "enum Color { red };",
            "int make(int);",
            "using fakestd::fake_abs;",
            "}",
            "namespace al = alpha;",
        ),
    );
    ws.write(
        "third/alpha/alpha/local.h",
        lines("#pragma once", "static int alpha_local() { return 2; }"),
    );
    // A copy per includer, initialized in each: a force-linking anchor's
    // point. A constant is only a value.
    ws.write("third/alpha/alpha/anchor.h", lines("#pragma once", "static int alpha_anchor = 0;"));
    ws.write(
        "third/alpha/alpha/limits.h",
        lines("#pragma once", "static const int alpha_max = 9;"),
    );
    ws.write(
        "third/beta/beta/beta.h",
        lines(
            "#pragma once",
            "#include <alpha/alpha.h>",
            "#include <fassert.h>",
            "namespace beta {",
            "inline alpha::Thing wrap(int v) { return alpha::Thing{ALPHA_TWICE(v)}; }",
            "}",
        ),
    );
    ws.write(
        "app/main.cpp",
        lines(
            "#define ALPHA_WIDE 1",
            "#define ALPHA_TMP 1",
            "#include <beta/beta.h>",
            "#include <alpha/local.h>",
            "#include <alpha/anchor.h>",
            "#include <alpha/limits.h>",
            "#include <fakecstdio>",
            "#undef ALPHA_TMP",
            "int main() {",
            "    fassert(1);",
            "    return beta::wrap(ALPHA_VERSION).v + alpha_local() + fake_stdout + FAKE_EOF +",
            '           fake_puts("x");',
            "}",
        ),
    );
    ws.write(
        "app/direct.cpp",
        lines("#include <cio.h>", "int direct() { return fake_stdout + FAKE_EOF; }"),
    );
    ws.write(
        "app/third.cpp",
        lines(
            "#include <cio.h>",
            "#include <fakecstdio>",
            "int third() { return fake_vprint(0); }",
        ),
    );
    const includes = ["std", "libc", "alpha", "beta"].map((dir) => `-I${ws.path(`third/${dir}`)}`);
    ws.writeEntries([
        ["app/main.cpp", includes],
        ["app/direct.cpp", includes],
        ["app/third.cpp", includes],
    ]);

    ws.write(
        "stdmod/std.cppm",
        lines(
            "module;",
            "#include <__config>",
            "#include <fakecstdio>",
            "#include <fakevector>",
            "#include <version>",
            "export module std;",
        ),
    );
    ws.write("stdmod/std.compat.cppm", lines("export module std.compat;"));
    ws.write(
        "stdmod/std.compat/cstdio.inc",
        lines("export {", "  using ::fake_puts _LIBCPP_USING_IF_EXISTS;", "} // export"),
    );
    ws.write(
        "partition.json",
        JSON.stringify({
            modules: [
                // An entry without a kind, the other naming it.
                { name: "libc", files: ["third/libc/cputs.h"] },
                { name: "std", files: ["third/std/**"], external: true },
                {
                    name: "libc",
                    files: ["third/libc/**"],
                    textual: true,
                    provides: "std.compat",
                },
                { name: "alpha", files: ["third/alpha/**"] },
                { name: "beta", files: ["third/beta/**"] },
            ],
        }),
    );
    const run = runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    return ws;
}

function interfaces(ws: Workspace): Map<string, Interface> {
    const run = runClice(
        "analyze",
        "modules",
        "--workspace",
        ws.root,
        "--scope",
        SCOPE,
        "--partition",
        ws.path("partition.json"),
        "--std",
        ws.path("stdmod"),
        "--view",
        "interface",
    );
    expect(run.status, `stdout: ${run.stdout}\nstderr: ${run.stderr}`).toBe(0);
    const list = JSON.parse(run.stdout) as Interface[];
    return new Map(list.map((entry) => [entry.module, entry]));
}

function modulize(ws: Workspace, partition = ws.path("partition.json")) {
    return runClice(
        "modulize",
        "--workspace",
        ws.root,
        "--scope",
        SCOPE,
        "--partition",
        partition,
        "--std",
        ws.path("stdmod"),
        "--out",
        ws.path("wrap"),
    );
}

test("library interfaces", ({ session }) => {
    const ws = writeProject(session);
    const all = interfaces(ws);
    const alpha = all.get("alpha")!;
    expect(alpha.entries.map((entry) => [entry.name, entry.include])).toEqual([
        ["alpha/alpha.h", "<alpha/alpha.h>"],
        ["alpha/limits.h", "<alpha/limits.h>"],
    ]);
    const exported = new Set(alpha.exports.map((entry) => entry.name));
    const wanted = ["alpha::Thing", "alpha::Color", "alpha::red", "alpha::make", "alpha::fake_abs"];
    expect(wanted.filter((name) => exported.has(name))).toEqual(wanted);
    // A hidden friend is found by argument-dependent lookup alone.
    expect(exported.has("alpha::operator==")).toBe(false);
    expect(alpha.aliases).toEqual([{ name: "al", target: "::alpha" }]);
    expect(alpha.macros.map((macro) => macro.name)).toEqual(["ALPHA_VERSION", "ALPHA_TWICE"]);
    // The program's switch ahead of the include; the one it undefines again
    // is no switch.
    expect(alpha.reads.map((macro) => macro.name)).toEqual(["ALPHA_WIDE"]);
    // An internal-linkage function the program calls and a static variable
    // each includer initializes: no interface exports them.
    expect(alpha.textual.map((header) => header.name)).toEqual(["alpha/anchor.h", "alpha/local.h"]);
    expect(all.get("beta")!.imports).toContain("alpha");
});

test("C library kept headers", ({ session }) => {
    const ws = writeProject(session);
    const all = interfaces(ws);
    const libc = all.get("libc")!;
    // main.cpp reaches <cio.h> only through <fakecstdio>, which `import std`
    // empties, while direct.cpp includes it itself; third.cpp's own <cio.h>
    // takes only the type of <cva.h>. fake_puts comes from std.compat.
    expect(libc.textual.map((header) => [header.include, header.because])).toEqual([
        ["<cio.h>", "fake_stdout in app/main.cpp"],
        ["<cva.h>", "fake_vprint in app/third.cpp"],
    ]);
    const macros = libc.macros.map((macro) => macro.name);
    expect(macros).toContain("FAKE_EOF");
    expect(macros).toContain("fake_stdout");
    // The #undef ahead of a definition ends nothing.
    expect(macros).toContain("fassert");
    expect(all.get("std")!.textual).toEqual([]);
});

test("modulize writes the wrapping", async ({ session }) => {
    const ws = writeProject(session);
    ws.write("wrap/mirror/stale/gone.h", "");
    ws.write("wrap/notes.txt", "kept");
    const run = modulize(ws);
    expect(run.status, `stdout: ${run.stdout}\nstderr: ${run.stderr}`).toBe(0);
    const plan = JSON.parse(run.stdout) as Plan;

    expect(plan.modules.map((module) => [module.name, module.imports, module.mirrors])).toEqual([
        ["alpha", [], ["mirror/std"]],
        ["beta", ["alpha"], ["mirror/std", "mirror/alpha"]],
    ]);
    const roots = plan.modules[0]!.includeRoots.map((root) => root.replaceAll("\\", "/"));
    expect(roots).toHaveLength(1);
    expect(roots[0]!.endsWith("/third/alpha")).toBe(true);
    expect(plan.stdSources).toHaveLength(2);
    expect(plan.mirrors).toEqual(["mirror/std", "mirror/alpha", "mirror/beta"]);
    expect(plan.warnings).toEqual([]);

    const alpha = ws.read("wrap/alpha.cppm");
    expect(alpha).toContain("import std.compat;");
    expect(alpha).toContain("export module alpha;");
    expect(alpha).toContain("export namespace alpha {\nusing ::alpha::Color;");
    expect(alpha).toContain("using ::alpha::red;");
    expect(alpha).not.toContain("operator==");
    expect(alpha).toContain("export namespace al = ::alpha;");
    const wide = alpha.indexOf("#define ALPHA_WIDE 1");
    expect(wide).toBeGreaterThan(-1);
    expect(wide).toBeLessThan(alpha.indexOf('/alpha/alpha.h"'));
    expect(alpha).not.toContain("ALPHA_TMP");
    const beta = ws.read("wrap/beta.cppm");
    expect(beta).toContain("import alpha;");
    expect(beta).toContain('#include "alpha.macros.h"');
    expect(beta).toContain("#include <alpha/local.h>");
    expect(ws.read("wrap/alpha.macros.h")).toContain("#define ALPHA_TWICE(x) ((x) * 2)");
    expect(ws.read("wrap/prelude.h")).toBe(
        lines(
            "#pragma once",
            "",
            "#include <cio.h>",
            "#include <cva.h>",
            "import std.compat;",
            '#include "std.macros.h"',
            '#include "libc.macros.h"',
            "import alpha;",
            "import beta;",
            '#include "alpha.macros.h"',
            '#include "beta.macros.h"',
        ),
    );

    // The headers importers see emptied: std.cppm's standard headers but
    // <version>, each library's entries but its textual headers.
    expect(ws.read("wrap/mirror/std/fakecstdio")).toBe("");
    expect(ws.exists("wrap/mirror/std/fakevector")).toBe(true);
    expect(ws.exists("wrap/mirror/std/version")).toBe(false);
    expect(ws.read("wrap/mirror/alpha/alpha/alpha.h")).toBe("");
    expect(ws.exists("wrap/mirror/alpha/alpha/limits.h")).toBe(true);
    expect(ws.exists("wrap/mirror/alpha/alpha/local.h")).toBe(false);
    expect(ws.exists("wrap/mirror/alpha/alpha/anchor.h")).toBe(false);
    expect(ws.exists("wrap/mirror/stale/gone.h")).toBe(false);
    expect(ws.read("wrap/notes.txt")).toBe("kept");

    // Unchanged files keep their timestamps.
    const before = statSync(ws.path("wrap/alpha.cppm")).mtimeMs;
    await sleep(MTIME_GRANULARITY);
    expect(modulize(ws).status).toBe(0);
    expect(statSync(ws.path("wrap/alpha.cppm")).mtimeMs).toBe(before);
});

test("modulize partition errors", ({ session }) => {
    const ws = writeProject(session);
    const failure = (run: ReturnType<typeof runClice>) => {
        expect(run.status, run.stdout).toBe(1);
        return (JSON.parse(run.stdout) as { error: string }).error;
    };
    const partition = (name: string, modules: unknown[]) => {
        ws.write(name, JSON.stringify({ modules }));
        return ws.path(name);
    };

    const external = partition("external.json", [
        { name: "alpha", files: ["third/alpha/**"], external: true },
    ]);
    expect(failure(modulize(ws, external))).toContain("only std stands for an existing module");

    // <fassert.h> in beta: alpha includes it, beta includes alpha.
    const cycle = partition("cycle.json", [
        { name: "beta", files: ["third/libc/fassert.h", "third/beta/**"] },
        { name: "std", files: ["third/std/**"], external: true },
        { name: "libc", files: ["third/libc/**"], textual: true, provides: "std.compat" },
        { name: "alpha", files: ["third/alpha/**"] },
    ]);
    expect(failure(modulize(ws, cycle))).toBe("modules import each other: alpha -> beta -> alpha");

    const both = partition("both.json", [
        { name: "libc", files: ["third/libc/cio.h"], textual: true },
        { name: "libc", files: ["third/libc/**"], external: true },
    ]);
    expect(failure(modulize(ws, both))).toContain("both textual and external");

    const noStd = runClice(
        "modulize",
        "--workspace",
        ws.root,
        "--scope",
        SCOPE,
        "--partition",
        ws.path("partition.json"),
        "--out",
        ws.path("wrap"),
    );
    expect(failure(noStd)).toContain("only std.compat, given --std");

    const badStd = runClice(
        "modulize",
        "--workspace",
        ws.root,
        "--partition",
        ws.path("partition.json"),
        "--std",
        ws.path("third/std"),
        "--out",
        ws.path("wrap"),
    );
    expect(failure(badStd)).toContain("cannot read");

    const missing = runClice("modulize", "--workspace", ws.root, "--out", ws.path("wrap"));
    expect(failure(missing)).toContain("--partition and --out");
});
