/// `clice analyze modules --view interface` and `clice modulize`: a
/// partition's libraries as module interface units over their headers, the
/// standard library as libc++'s std.compat, and the C library kept headers.
/// The libraries, the standard library and its module sources are stand-ins
/// in the workspace, so no real system header is involved.

import { spawnSync } from "node:child_process";
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
    out: string;
    stdSources: string[];
    modules: { name: string; source: string; imports: string[]; includeRoots: string[] }[];
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
        ),
    );
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
            "namespace alpha {",
            "struct Thing { int v; };",
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
            "#include <beta/beta.h>",
            "#include <alpha/local.h>",
            "#include <fakecstdio>",
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
    const includes = ["std", "libc", "alpha", "beta"].map((dir) => `-I${ws.path(`third/${dir}`)}`);
    ws.writeEntries([
        ["app/main.cpp", includes],
        ["app/direct.cpp", includes],
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
    ]);
    const exported = new Set(alpha.exports.map((entry) => entry.name));
    expect(
        ["alpha::Thing", "alpha::make", "alpha::fake_abs"].filter((n) => exported.has(n)),
    ).toEqual(["alpha::Thing", "alpha::make", "alpha::fake_abs"]);
    expect(alpha.aliases).toEqual([{ name: "al", target: "alpha" }]);
    expect(alpha.macros.map((macro) => macro.name)).toEqual(["ALPHA_VERSION", "ALPHA_TWICE"]);
    // An internal-linkage function the program calls: no interface exports
    // it, so its header stays textual.
    expect(alpha.textual.map((header) => header.name)).toEqual(["alpha/local.h"]);
    expect(all.get("beta")!.imports).toContain("alpha");
});

test("C library kept headers", ({ session }) => {
    const ws = writeProject(session);
    const libc = interfaces(ws).get("libc")!;
    // main.cpp reaches <cio.h> only through <fakecstdio>, which `import std`
    // empties; direct.cpp includes it itself. fake_puts comes from
    // std.compat, so <cputs.h> stays out.
    expect(libc.textual.map((header) => [header.include, header.because])).toEqual([
        ["<cio.h>", "fake_stdout in app/main.cpp"],
    ]);
    const macros = libc.macros.map((macro) => macro.name);
    expect(macros).toContain("FAKE_EOF");
    expect(macros).toContain("fake_stdout");
    // The #undef ahead of a definition ends nothing.
    expect(macros).toContain("fassert");
    expect(interfaces(ws).get("std")!.textual).toEqual([]);
});

test("modulize writes the wrapping", ({ session }) => {
    const ws = writeProject(session);
    ws.write("wrap/mirror/stale/gone.h", "");
    const run = modulize(ws);
    expect(run.status, `stdout: ${run.stdout}\nstderr: ${run.stderr}`).toBe(0);
    const plan = JSON.parse(run.stdout) as Plan;

    expect(plan.modules.map((module) => [module.name, module.imports])).toEqual([
        ["alpha", []],
        ["beta", ["alpha"]],
    ]);
    const roots = plan.modules[0]!.includeRoots.map((root) => root.replaceAll("\\", "/"));
    expect(roots).toHaveLength(1);
    expect(roots[0]!.endsWith("/third/alpha")).toBe(true);
    expect(plan.stdSources).toHaveLength(2);
    expect(plan.mirrors).toEqual(["mirror/std", "mirror/alpha", "mirror/beta"]);

    const alpha = ws.read("wrap/alpha.cppm");
    expect(alpha).toContain("import std.compat;");
    expect(alpha).toContain("export module alpha;");
    expect(alpha).toContain("export namespace alpha {\nusing ::alpha::Thing;");
    expect(alpha).toContain("export namespace al = alpha;");
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
    expect(ws.exists("wrap/mirror/alpha/alpha/local.h")).toBe(false);
    expect(ws.exists("wrap/mirror/stale/gone.h")).toBe(false);
});

test("modulize partition errors", ({ session }) => {
    const ws = writeProject(session);
    ws.write(
        "other.json",
        JSON.stringify({ modules: [{ name: "alpha", files: ["third/alpha/**"], external: true }] }),
    );
    const run = modulize(ws, ws.path("other.json"));
    expect(run.status).toBe(1);
    expect((JSON.parse(run.stdout) as { error: string }).error).toContain(
        "only std stands for an existing module",
    );

    const missing = runClice("modulize", "--workspace", ws.root, "--out", ws.path("wrap"));
    expect(missing.status).toBe(1);
    expect((JSON.parse(missing.stdout) as { error: string }).error).toContain(
        "--partition and --out",
    );
});
