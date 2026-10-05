/// `clice analyze modules --view interface` and `clice modularize`: a
/// partition's libraries as module interface units over their headers, the
/// standard library as libc++'s std.compat, the C library kept headers, and
/// the program's own modules rewritten into module units.
/// The libraries, the standard library and its module sources are stand-ins
/// in the workspace, so no real system header is involved.

import { statSync } from "node:fs";
import { MTIME_GRANULARITY, type ProcessResult, runProcess, sleep } from "@clice/tools/client";
import { type Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test, type SessionFactory } from "../fixtures.ts";

interface Header {
    file: string;
    names: string[];
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

interface Rewriting {
    modules: {
        name: string;
        primary: string;
        interfaces: string[];
        partitions: string[];
        sources: string[];
        imports: string[];
    }[];
    importers: string[];
    macros: string[];
    removed: string[];
    moved: string[];
    warnings: string[];
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

function runClice(...args: string[]): Promise<ProcessResult> {
    return runProcess(cliceExecutable(), args, { timeout: 120_000 });
}

/// A program over two libraries, beta including alpha, a standard library
/// whose C++ headers include the C library's, and libc++'s module sources
/// for it: std.cppm includes the headers `import std` stands for and
/// std.compat exports fake_puts. With `program`, the program also has two
/// modules of its own, core and tool, and a source using tool.
async function writeProject(session: SessionFactory, program = false): Promise<Workspace> {
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
            "#define ALPHA_EXTERN extern",
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
            "#include <alpha/local.h>",
            "#include <fassert.h>",
            "inline int beta_local() { return alpha_local(); }",
            "namespace beta {",
            "inline alpha::Thing wrap(int v) { return alpha::Thing{ALPHA_TWICE(v)}; }",
            "#define BETA_COUNTER(name) ALPHA_EXTERN int name;",
            '#include "counters.inc"',
            "#undef BETA_COUNTER",
            "}",
        ),
    );
    // Only the header pasting it expands its macro.
    ws.write("third/beta/beta/counters.inc", lines("BETA_COUNTER(opened)"));
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
    const sources = ["app/main.cpp", "app/direct.cpp", "app/third.cpp"];
    if (program) {
        writeProgram(ws);
        includes.push(`-I${ws.path("app")}`);
        sources.push(
            "app/core/text.cpp",
            "app/tool/tool.cpp",
            "app/tool/hook.cpp",
            "app/tool/cli.cpp",
            "app/run.cpp",
        );
    }
    ws.writeEntries(sources.map((source): [string, string[]] => [source, includes]));

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
    const run = await runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    return ws;
}

/// core's text.h forward-declares sink.h's Sink and declares hook(), which
/// tool's hook.cpp defines; detail.h is core's alone. tool.h forward-declares
/// core's Sink, and tool.cpp expands core's CORE_TWICE.
function writeProgram(ws: Workspace): void {
    ws.write(
        "app/core/text.h",
        lines(
            "#pragma once",
            "#include <alpha/alpha.h>",
            "#define CORE_TWICE(x) ((x) * 2)",
            "namespace core {",
            "struct Sink;",
            "struct Text {",
            "    alpha::Thing thing;",
            "    int size() const;",
            "};",
            "int hook();",
            "}",
        ),
    );
    ws.write(
        "app/core/sink.h",
        lines("#pragma once", "namespace core {", "struct Sink { int lines; };", "}"),
    );
    ws.write(
        "app/core/detail.h",
        lines(
            "#pragma once",
            "namespace core {",
            "namespace {",
            "inline int hidden() { return 1; }",
            "}",
            "inline int detail() { return hidden(); }",
            "}",
        ),
    );
    ws.write(
        "app/core/text.cpp",
        lines(
            '#include "core/text.h"',
            '#include "core/detail.h"',
            "int core::Text::size() const { return detail() + CORE_TWICE(thing.v); }",
        ),
    );
    ws.write(
        "app/tool/tool.h",
        lines(
            "#pragma once",
            '#include "core/text.h"',
            "namespace core {",
            "struct Sink;",
            "}",
            "namespace tool {",
            "int run(const core::Text& text, core::Sink* sink);",
            "}",
        ),
    );
    ws.write(
        "app/tool/tool.cpp",
        lines(
            '#include "tool/tool.h"',
            '#include "core/sink.h"',
            "int tool::run(const core::Text& text, core::Sink* sink) {",
            "    return text.size() + sink->lines + CORE_TWICE(1);",
            "}",
        ),
    );
    ws.write(
        "app/tool/hook.cpp",
        lines('#include "core/text.h"', "int core::hook() { return 7; }"),
    );
    ws.write(
        "app/tool/cli.cpp",
        lines(
            '#include "tool/tool.h"',
            "int main() {",
            '    const char* code = R"(int main() {})";',
            "    return code[0] == 'i' ? 0 : tool::run(core::Text{}, nullptr);",
            "}",
        ),
    );
    ws.write(
        "app/run.cpp",
        lines('#include "tool/tool.h"', "int main() { return tool::run(core::Text{}, nullptr); }"),
    );
}

async function interfaces(ws: Workspace): Promise<Map<string, Interface>> {
    const run = await runClice(
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

function modularize(ws: Workspace, partition = ws.path("partition.json")) {
    return runClice(
        "modularize",
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

test("library interfaces", async ({ session }) => {
    const ws = await writeProject(session);
    const all = await interfaces(ws);
    const alpha = all.get("alpha")!;
    expect(alpha.entries.map((entry) => [entry.names, entry.include])).toEqual([
        [["alpha/alpha.h"], "<alpha/alpha.h>"],
        [["alpha/limits.h"], "<alpha/limits.h>"],
    ]);
    const exported = new Set(alpha.exports.map((entry) => entry.name));
    const wanted = ["alpha::Thing", "alpha::Color", "alpha::red", "alpha::make", "alpha::fake_abs"];
    expect(wanted.filter((name) => exported.has(name))).toEqual(wanted);
    // A hidden friend is found by argument-dependent lookup alone.
    expect(exported.has("alpha::operator==")).toBe(false);
    expect(alpha.aliases).toEqual([{ name: "al", target: "::alpha" }]);
    // ALPHA_EXTERN through a macro of beta's that only a fragment beta.h
    // pastes expands.
    expect(alpha.macros.map((macro) => macro.name)).toEqual([
        "ALPHA_VERSION",
        "ALPHA_TWICE",
        "ALPHA_EXTERN",
    ]);
    // The program's switch ahead of the include; the one it undefines again
    // is no switch.
    expect(alpha.reads.map((macro) => macro.name)).toEqual(["ALPHA_WIDE"]);
    // An internal-linkage function the program calls and a static variable
    // each includer initializes: no interface exports them.
    expect(alpha.textual.map((header) => header.names)).toEqual([
        ["alpha/anchor.h"],
        ["alpha/local.h"],
    ]);
    expect(all.get("beta")!.imports).toContain("alpha");
});

test("C library kept headers", async ({ session }) => {
    const ws = await writeProject(session);
    const all = await interfaces(ws);
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

test("modularize writes the wrapping", async ({ session }) => {
    const ws = await writeProject(session);
    ws.write("wrap/custom.cppm", "export module custom;\n");
    const run = await modularize(ws);
    expect(run.status, `stdout: ${run.stdout}\nstderr: ${run.stderr}`).toBe(0);
    const plan = (JSON.parse(run.stdout) as { wrapping: Plan }).wrapping;

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
    expect(wide).toBeLessThan(alpha.indexOf("#include <alpha/alpha.h>"));
    expect(alpha).not.toContain("ALPHA_TMP");
    const beta = ws.read("wrap/beta.cppm");
    expect(beta).toContain("import alpha;");
    expect(beta).toContain('#include "alpha.macros.h"');
    // What beta's headers name of alpha's textual headers, not every one.
    expect(beta).toContain("#include <alpha/local.h>");
    expect(beta).not.toContain("anchor.h");
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
    expect(ws.read("wrap/custom.cppm")).toBe("export module custom;\n");

    // Unchanged files keep their timestamps.
    const before = statSync(ws.path("wrap/alpha.cppm")).mtimeMs;
    await sleep(MTIME_GRANULARITY);
    expect((await modularize(ws)).status).toBe(0);
    expect(statSync(ws.path("wrap/alpha.cppm")).mtimeMs).toBe(before);

    // Without beta, what the last run wrote for it goes; the rest stays.
    ws.write(
        "alpha.json",
        JSON.stringify({
            modules: [
                { name: "std", files: ["third/std/**"], external: true },
                { name: "libc", files: ["third/libc/**"], textual: true, provides: "std.compat" },
                { name: "alpha", files: ["third/alpha/**"] },
            ],
        }),
    );
    expect((await modularize(ws, ws.path("alpha.json"))).status).toBe(0);
    expect(ws.exists("wrap/beta.cppm")).toBe(false);
    expect(ws.exists("wrap/mirror/beta/beta/beta.h")).toBe(false);
    expect(ws.exists("wrap/alpha.cppm")).toBe(true);
    expect(ws.exists("wrap/custom.cppm")).toBe(true);
});

test("modularize rewrites program modules", async ({ session }) => {
    const ws = await writeProject(session, true);
    const partition = JSON.parse(ws.read("partition.json")) as { modules: unknown[] };
    partition.modules.push(
        { name: "app.core", files: ["app/core/**"], rewrite: true, primary: "app/core/core.cppm" },
        { name: "app.tool", files: ["app/tool/**"], rewrite: true },
    );
    ws.write("program.json", JSON.stringify(partition));
    const run = await modularize(ws, ws.path("program.json"));
    expect(run.status, `stdout: ${run.stdout}\nstderr: ${run.stderr}`).toBe(0);
    const plan = (JSON.parse(run.stdout) as { rewriting: Rewriting }).rewriting;

    expect(plan.modules).toEqual([
        {
            name: "app.core",
            primary: "app/core/core.cppm",
            interfaces: ["app/core/sink.cppm", "app/core/text.cppm"],
            partitions: ["app/core/detail.cppm"],
            sources: ["app/core/text.cpp", "app/tool/hook.cpp"],
            imports: [],
        },
        {
            name: "app.tool",
            primary: "app/tool/module.cppm",
            interfaces: ["app/tool/tool.cppm"],
            partitions: [],
            sources: ["app/tool/cli.cpp", "app/tool/tool.cpp"],
            imports: ["app.core"],
        },
    ]);
    expect(plan.importers).toEqual(["app/run.cpp"]);
    // Defining what core's text.h declares, it is attached to core.
    expect(plan.moved).toEqual(["app/tool/hook.cpp=app.core"]);
    expect(plan.macros).toEqual(["app/core/text.macros.h"]);
    expect(plan.removed).toEqual([
        "app/core/detail.h",
        "app/core/sink.h",
        "app/core/text.h",
        "app/tool/tool.h",
    ]);
    expect(plan.warnings).toEqual([]);
    expect(ws.exists("app/core/text.h")).toBe(false);

    expect(ws.read("app/core/core.cppm")).toBe(
        lines("export module app.core;", "", "export import :sink;", "export import :text;"),
    );
    expect(ws.read("app/tool/module.cppm")).toBe(
        lines("export module app.tool;", "", "export import :tool;"),
    );
    // The prelude imports alpha; core's own forward declaration stays.
    expect(ws.read("app/core/text.cppm")).toBe(
        lines(
            "module;",
            "",
            '#include "wrap/prelude.h"',
            "#define CORE_TWICE(x) ((x) * 2)",
            "",
            "export module app.core:text;",
            "",
            "export {",
            "namespace core {",
            "struct Sink;",
            "struct Text {",
            "    alpha::Thing thing;",
            "    int size() const;",
            "};",
            "int hook();",
            "}",
            "}",
        ),
    );
    // An implementation partition, its anonymous namespace dissolved.
    const detail = ws.read("app/core/detail.cppm");
    expect(detail).toContain("\nmodule app.core:detail;\n");
    expect(detail).not.toContain("namespace {");
    expect(detail).toContain("inline int hidden() { return 1; }");
    // The interface partition comes through the primary interface, the
    // implementation partition by import; the macro through its header.
    const text = ws.read("app/core/text.cpp");
    expect(text).toContain("\nmodule app.core;\n");
    expect(text).toContain("import :detail;");
    expect(text).not.toContain("import :text;");
    expect(text).toContain('#include "core/text.macros.h"');
    expect(ws.read("app/core/text.macros.h")).toBe(
        lines("#pragma once", "", "#define CORE_TWICE(x) ((x) * 2)"),
    );
    // core's Sink is core's to declare.
    const tool = ws.read("app/tool/tool.cppm");
    expect(tool).toContain("export module app.tool:tool;");
    expect(tool).toContain("import app.core;");
    expect(tool).not.toContain("struct Sink;");
    expect(ws.read("app/tool/tool.cpp")).toContain('#include "core/text.macros.h"');
    expect(ws.read("app/tool/hook.cpp")).toContain("\nmodule app.core;\n");
    // main stays attached to the global module; the one in a string is text.
    const cli = ws.read("app/tool/cli.cpp");
    expect(cli).toContain('extern "C++" int main() {');
    expect(cli).toContain('R"(int main() {})"');
    // core::Text reaches it through tool's header, but no import re-exports.
    expect(ws.read("app/run.cpp")).toBe(
        lines(
            '#include "wrap/prelude.h"',
            "import app.core;",
            "import app.tool;",
            "",
            "int main() { return tool::run(core::Text{}, nullptr); }",
        ),
    );
});

test("modularize partition errors", async ({ session }) => {
    const ws = await writeProject(session);
    const failure = (run: ProcessResult) => {
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
    expect(failure(await modularize(ws, external))).toContain(
        "only std stands for an existing module",
    );

    // <fassert.h> in beta: alpha includes it, beta includes alpha.
    const cycle = partition("cycle.json", [
        { name: "beta", files: ["third/libc/fassert.h", "third/beta/**"] },
        { name: "std", files: ["third/std/**"], external: true },
        { name: "libc", files: ["third/libc/**"], textual: true, provides: "std.compat" },
        { name: "alpha", files: ["third/alpha/**"] },
    ]);
    expect(failure(await modularize(ws, cycle))).toBe(
        "modules import each other: alpha -> beta -> alpha",
    );

    const named = partition("named.json", [{ name: "../escape", files: ["third/alpha/**"] }]);
    expect(failure(await modularize(ws, named))).toContain("not a module name");

    const flags = partition("flags.json", [
        { name: "std", files: ["third/std/**"], textual: true, external: true },
    ]);
    expect(failure(await modularize(ws, flags))).toContain("both textual and external");

    const both = partition("both.json", [
        { name: "libc", files: ["third/libc/cio.h"], textual: true },
        { name: "libc", files: ["third/libc/**"], external: true },
    ]);
    expect(failure(await modularize(ws, both))).toContain("both textual and external");

    const rewritten = partition("rewritten.json", [
        { name: "alpha", files: ["third/alpha/**"], textual: true, rewrite: true },
    ]);
    expect(failure(await modularize(ws, rewritten))).toContain("both rewritten and wrapped");
    const wrapped = partition("wrapped.json", [
        { name: "alpha", files: ["third/alpha/alpha.h"], rewrite: true },
        { name: "alpha", files: ["third/alpha/**"] },
    ]);
    expect(failure(await modularize(ws, wrapped))).toContain("both rewritten and wrapped");
    const primary = partition("primary.json", [
        { name: "alpha", files: ["third/alpha/**"], primary: "third/alpha/alpha.cppm" },
    ]);
    expect(failure(await modularize(ws, primary))).toContain("not rewritten");

    const noStd = await runClice(
        "modularize",
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

    const badStd = await runClice(
        "modularize",
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

    const missing = await runClice("modularize", "--workspace", ws.root, "--out", ws.path("wrap"));
    expect(failure(missing)).toContain("--partition and --out");
});
