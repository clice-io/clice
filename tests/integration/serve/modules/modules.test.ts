/// Integration tests for C++20 module support.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

/// Loose files built as C++20 units, each with its own arguments.
function cxx20(units: Record<string, string[]>) {
    return { manifest: { cxx: ["-std=c++20"], units } };
}

/// The names the index lists for `query`.
async function names(s: Serve, query: string): Promise<string[]> {
    return ((await s.workspaceSymbols(query)) ?? []).map((symbol) => symbol.name);
}

/// The lines of a rendered reply that point into `file`.
function into(s: Serve, reply: unknown, file: string): string[] {
    return s
        .show(reply)
        .split("\n")
        .filter((line) => line.startsWith(`${file}: `));
}

serve.data("modules/single_module_no_deps")("single module no deps", async ({ s }) => {
    await s.clean("mod_a.cppm");
});

/// Opening mod_b that imports mod_a should trigger dependency compilation.
serve.data("modules/chained_modules")("chained modules", async ({ s }) => {
    await s.clean("mod_b.cppm");
});

serve.data("modules/diamond_modules")("diamond modules", async ({ s }) => {
    await s.clean("top.cppm");
});

serve.data("modules/dotted_module_name")("dotted module name", async ({ s }) => {
    await s.clean("app.cppm");
});

/// Implementation unit (module M; without export) should compile using the
/// interface PCM.
serve.data("modules/module_implementation_unit")("module implementation unit", async ({ s }) => {
    await s.clean("greeter_impl.cpp");
});

/// A regular .cpp that imports a module should get PCM deps compiled first.
serve.data("modules/consumer_imports_module")("consumer imports module", async ({ s }) => {
    await s.clean("main.cpp");
});

/// Partitions should be compiled in correct dependency order.
serve.data("modules/module_partitions")("module partitions", async ({ s }) => {
    await s.clean("lib.cppm");
});

serve.data("modules/partition_interface")("partition interface", async ({ s }) => {
    await s.clean("primary.cppm");
});

serve.data("modules/partition_chain")("partition chain", async ({ s }) => {
    await s.clean("sys.cppm");
});

/// Internal partitions (`module M:part;`) import other partitions and are
/// imported by the other units of M, among them an implementation unit,
/// which imports the primary interface implicitly.
serve.data("modules/internal_partitions")("internal partitions", async ({ s }) => {
    for (const file of [
        "detail.cppm",
        "util.cppm",
        "api.cppm",
        "lib.cppm",
        "impl.cpp",
        "main.cpp",
    ]) {
        await s.clean(file);
    }
});

serve.data("modules/internal_partitions")("internal partition definition", async ({ s }) => {
    await s.compiled("impl.cpp");
    await s.indexed();
    expect(await names(s, "Lib:util"), "Index not ready").toContain("Lib:util");
    expect(into(s, await s.definition(at("impl.cpp", "import :|util;")), "util.cppm")).toEqual([
        "util.cppm: module Lib:util;",
    ]);
});

const PICK_V1 = "export module m;\nexport int pick(int v) { return v; }\n";

// Adds an overload the closed importer's call prefers: only a reindex of
// the importer against the new interface points the call at it.
const PICK_V2 =
    "export module m;\nexport int pick(int v) { return v; }\nexport int pick(long v) { return 2; }\n";

serve.files(
    { "m.cppm": PICK_V1, "closed.cpp": "import m;\nint use() { return pick(1L); }\n" },
    cxx20({ "m.cppm": [], "closed.cpp": [] }),
)("module save reindexes importers", async ({ s }) => {
    const call = "closed.cpp: int use() { return pick(1L); }";
    await s.compiled("m.cppm");
    await s.indexed();
    expect(
        into(s, await s.references(at("m.cppm", "int |pick(int")), "closed.cpp"),
        "initial index never produced the importer's pick(int) reference",
    ).toEqual([call]);

    // The next server's startup sweep finds the importer fresh and never
    // runs it: only the index's record of what it imported leads the save
    // to it.
    await s.stop();
    await s.start({ config: { project: { idle_timeout_ms: 10 } } });
    await s.compiled("m.cppm");
    // The rewrite must stat newer than the text the importer was indexed against.
    await sleep(MTIME_GRANULARITY);
    s.edit("m.cppm", { text: PICK_V2 });
    s.save("m.cppm");
    await s.sync();

    expect(
        into(s, await s.references(at("m.cppm", "int |pick(long")), "closed.cpp"),
        "the importer was not reindexed after the module save",
    ).toEqual([call]);
    expect(into(s, await s.references(at("m.cppm", "int |pick(int")), "closed.cpp")).toEqual([]);
});

/// Re-exported symbols (export import) should be accessible through the wrapper.
serve.data("modules/re_export")("re export", async ({ s }) => {
    await s.clean("user.cppm");
});

serve.data("modules/export_block")("export block", async ({ s }) => {
    await s.clean("consumer.cppm");
});

serve.data("modules/global_module_fragment")("global module fragment", async ({ s }) => {
    await s.clean("gmf.cppm");
});

serve.data("modules/private_module_fragment")("private module fragment", async ({ s }) => {
    await s.clean("priv.cppm");
});

serve.data("modules/export_namespace")("export namespace", async ({ s }) => {
    await s.clean("calc.cppm");
});

serve.data("modules/gmf_with_import")("gmf with import", async ({ s }) => {
    await s.clean("combined.cppm");
});

serve.data("modules/independent_modules")("independent modules", async ({ s }) => {
    await s.clean("x.cppm");
    await s.clean("y.cppm");
});

serve.data("modules/template_export")("template export", async ({ s }) => {
    await s.clean("use_tmpl.cppm");
});

serve.data("modules/class_export_and_inheritance")(
    "class export and inheritance",
    async ({ s }) => {
        await s.clean("circle.cppm");
    },
);

/// Closing and reopening a modified module file should recompile without errors.
serve.data("modules/save_recompile")("save recompile", async ({ s }) => {
    // Open and compile Mid (which triggers Leaf PCM build).
    await s.clean("mid.cppm");
    await s.compiled("leaf.cppm");

    // Close Leaf, modify on disk, and reopen with new content.
    s.close("leaf.cppm");
    s.disk.write("leaf.cppm", "export module Leaf;\nexport int leaf() { return 100; }\n");
    await s.clean("leaf.cppm");
});

serve.data("modules/module_compile_error")("module compile error", async ({ s }) => {
    const diagnostics = await s.compiled("bad.cppm");
    expect(diagnostics.length, "Expected diagnostics for undefined symbol").toBeGreaterThan(0);
    const errors = diagnostics.filter((d) => d.severity === proto.DiagnosticSeverity.Error);
    expect(
        s.show(errors.map(({ range }) => ({ uri: s.uri("bad.cppm"), range }))).split("\n"),
    ).toContain("bad.cppm: return UNDEFINED_SYMBOL;");
});

/// An import whose interface fails to build is reported on the import:
/// the importer still publishes, and follows its own edits.
serve.files(
    {
        "a.cppm": "export module A;\nexport int a() { return broken_in_a; }\n",
        "main.cpp": "import A;\nint main() { return a(); }\n",
    },
    cxx20({ "a.cppm": [], "main.cpp": [] }),
)("failed import reported", async ({ s }) => {
    const importErrors = async () =>
        (await s.errors("main.cpp")).filter(
            (d) => d.code === "err_module_not_found" && d.range.start.line === 0,
        );
    expect(await importErrors()).toHaveLength(1);

    s.edit("main.cpp", { replace: "a()", with: "a() + 1" });
    expect(await importErrors()).toHaveLength(1);
    expect((await s.counts()).files["main.cpp"]?.publish, "one publish per compile").toBe(2);
});

/// A module whose build fails is not rebuilt for its importer's edits: the
/// same inputs fail again. Saving a fix is a new input.
const MAIN_PLUS = (n: number) => `import A;\nint main() { return a() + ${n}; }\n`;
serve.files(
    {
        "a.cppm": "export module A;\nexport int a() { return broken_in_a; }\n",
        "main.cpp": MAIN_PLUS(0),
    },
    cxx20({ "a.cppm": [], "main.cpp": [] }),
)("failed module waits for its inputs", async ({ s }) => {
    await s.compiled("main.cpp");
    for (let n = 1; n <= 3; n++) {
        s.edit("main.cpp", { text: MAIN_PLUS(n) });
        await s.compiled("main.cpp");
    }
    expect((await s.counts()).files["a.cppm"]?.pcm, "the failed build ran once").toBe(1);

    s.disk.write("a.cppm", "export module A;\nexport int a() { return 1; }\n");
    s.save("a.cppm");
    await s.clean("main.cpp");
    expect((await s.counts()).files["a.cppm"]?.pcm, "the fix built once").toBe(2);
});

/// A module that failed for what an interface it imports lacked builds
/// again once that interface changes.
serve.files(
    {
        "b.cppm": "export module B;\nexport int b() { return 1; }\n",
        "a.cppm": "export module A;\nimport B;\nexport int a() { return c(); }\n",
        "main.cpp": "import A;\nint main() { return a(); }\n",
    },
    cxx20({ "b.cppm": [], "a.cppm": [], "main.cpp": [] }),
)("failed module follows its import", async ({ s }) => {
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);

    s.disk.write("b.cppm", "export module B;\nexport int c() { return 1; }\n");
    s.save("b.cppm");
    await s.clean("main.cpp");
});

/// A module that failed on an import nothing provided builds again once a
/// unit declares that module.
serve.files(
    {
        "m.cppm": "int placeholder;\n",
        "a.cppm": "export module A;\nimport M;\nexport int a() { return m(); }\n",
        "main.cpp": "import A;\nint main() { return a(); }\n",
    },
    cxx20({ "m.cppm": [], "a.cppm": [], "main.cpp": [] }),
)("failed module follows a new provider", async ({ s }) => {
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);

    s.disk.write("m.cppm", "export module M;\nexport int m() { return 1; }\n");
    s.save("m.cppm");
    await s.clean("main.cpp");
});

/// A header that shows up in a search directory missing at the build is no
/// input the failed module build recorded: saving an importer retries it,
/// through the modules in between.
serve.files(
    {
        "b.cppm":
            'module;\n#include "generated.h"\nexport module B;\nexport int b() { return generated(); }\n',
        "a.cppm": "export module A;\nimport B;\nexport int a() { return b(); }\n",
        "main.cpp": "import A;\nint main() { return a(); }\n",
    },
    cxx20({ "b.cppm": ["-Igen"], "a.cppm": [], "main.cpp": [] }),
)("failed module retries on save", async ({ s }) => {
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);

    s.disk.write("gen/generated.h", "#pragma once\ninline int generated() { return 1; }\n");
    s.save("main.cpp");
    await s.clean("main.cpp");
});

/// An import reaching the unit only through its command's forced include
/// is built before the unit compiles.
serve.files(
    {
        "a.cppm": "export module A;\nexport int a() { return 1; }\n",
        "deps.h": "import A;\n",
        "main.cpp": "int main() { return a(); }\n",
    },
    cxx20({ "a.cppm": [], "main.cpp": ["-include", "deps.h"] }),
)("forced include imports module", async ({ s }) => {
    await s.clean("main.cpp");
});

/// An unsaved include of a header that imports builds the module first.
serve.files(
    {
        "a.cppm": "export module A;\nexport int a() { return 1; }\n",
        "deps.h": "import A;\n",
        "main.cpp": "int main() { return 0; }\n",
    },
    cxx20({ "a.cppm": [], "main.cpp": [] }),
)("unsaved include imports module", async ({ s }) => {
    await s.compiled("main.cpp");
    s.edit("main.cpp", { text: '#include "deps.h"\nint main() { return a(); }\n' });
    await s.clean("main.cpp");
});

/// A preamble that imports keeps its PCH, rebuilt against a rebuilt module.
serve.files(
    {
        "a.cppm": "export module A;\nexport int a() { return 1; }\n",
        "deps.h": "import A;\n",
        "main.cpp": '#include "deps.h"\nint main() { return a(); }\n',
    },
    cxx20({ "a.cppm": [], "main.cpp": [] }),
)("preamble import keeps its pch", async ({ s }) => {
    const stderr = () => s.client.drainedStderr().toString("utf8");
    await s.clean("main.cpp");
    await s.completion(at("main.cpp", "int main"));
    expect((await s.counts()).pch).toBe(1);

    s.disk.write(
        "a.cppm",
        "export module A;\nexport int b() { return 2; }\nexport int a() { return 1; }\n",
    );
    s.save("a.cppm");
    await s.compiled("main.cpp");
    s.edit("main.cpp", { replace: "a()", with: "a() + b()" });
    await s.clean("main.cpp");
    expect((await s.counts()).pch).toBe(2);
    expect(stderr()).not.toContain("PCH build failed");
    expect(stderr()).not.toContain("blamed PCH pair");
});

/// A preamble import that resolves to nothing gets its module once a
/// provider appears.
serve.files(
    {
        "a.cppm": "int placeholder;\n",
        "deps.h": "import A;\n",
        "main.cpp": '#include "deps.h"\nint main() { return a(); }\n',
    },
    cxx20({ "a.cppm": [], "main.cpp": [] }),
)("preamble import finds a provider", async ({ s }) => {
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);

    s.disk.write("a.cppm", "export module A;\nexport int a() { return 1; }\n");
    s.save("a.cppm");
    await s.clean("main.cpp");
});

/// A PCH would lose the imports of a module unit's global module fragment;
/// a fragment that only includes keeps its PCH.
serve.files(
    {
        "a.cppm": "export module A;\nexport int a() { return 1; }\n",
        "deps.h": "import A;\n",
        "plain.h": "inline int plain() { return 2; }\n",
        "m.cppm": 'module;\n#include "deps.h"\nexport module M;\nexport int m() { return a(); }\n',
        "n.cppm":
            'module;\n#include "plain.h"\nexport module N;\nexport int n() { return plain(); }\n',
    },
    cxx20({ "a.cppm": [], "m.cppm": [], "n.cppm": [] }),
)("fragment import skips the pch", async ({ s }) => {
    await s.clean("m.cppm");
    await s.clean("n.cppm");
    await s.sync();
    const counts = await s.counts();
    expect(counts.files["n.cppm"]?.pch, "the plain fragment's PCH").toBe(1);
    expect(counts.pch).toBe(1);
    expect(s.client.drainedStderr().toString("utf8")).not.toContain("PCH build failed");
});

/// A module whose own import is missing breaks its importers' import too.
serve.files(
    {
        "a.cppm": "export module A;\nimport missing;\n",
        "b.cppm": "export module B;\nimport A;\n",
    },
    cxx20({ "a.cppm": [], "b.cppm": [] }),
)("nested missing module", async ({ s }) => {
    expect(
        (await s.errors("b.cppm")).map((d) => `${d.range.start.line} ${String(d.code)}`),
    ).toEqual(["1 err_module_not_found"]);
});

/// A 5-level module chain (m1->m2->...->m5) should compile correctly.
serve.data("modules/deep_chain")("deep chain", async ({ s }) => {
    await s.clean("m5.cppm");
});

serve.data("modules/partition_with_gmf")("partition with gmf", async ({ s }) => {
    await s.clean("cfg.cppm");
});

serve.data("modules/partition_with_external_import")(
    "partition with external import",
    async ({ s }) => {
        await s.clean("app.cppm");
    },
);

/// Hover on a symbol imported from a module should return type info.
serve.data("modules/hover_on_imported_symbol")("hover on imported symbol", async ({ s }) => {
    await s.clean("use.cpp");
    const hover = await s.hover(at("use.cpp", "return |magic_number"));
    expect(hover, "Hover on imported symbol should return info").not.toBeNull();
    expect(hover!.contents).not.toBeNull();
});

/// Plain .cpp with no modules should compile normally (CompileGraph null path).
serve.data("modules/no_modules_plain_cpp")("no modules plain cpp", async ({ s }) => {
    await s.clean("plain.cpp");
});

/// Circular module imports should not hang the server.
///
/// The CompileGraph's cycle detection should prevent deadlock. We verify
/// the server settles and stays responsive by opening a non-cyclic file
/// afterwards.
serve.data("modules/circular_module_dependency")("circular module dependency", async ({ s }) => {
    s.open("cycle_a.cppm");
    await s.sync();
    await s.clean("ok.cppm");
});

serve.data("modules/consumer_imports_module")("import definition", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();
    expect(await names(s, "Math"), "Index not ready").toContain("Math");
    expect(into(s, await s.definition(at("main.cpp", "import M|ath;")), "math.cppm")).toEqual([
        "math.cppm: export module Math;",
    ]);

    // Cursor on the `import` keyword itself must not navigate to the module.
    expect(into(s, await s.definition(at("main.cpp", "im|port Math;")), "math.cppm")).toEqual([]);
});

serve.data("modules/module_implementation_unit")("module decl definition", async ({ s }) => {
    // `module Greeter;` in the implementation unit navigates to the interface.
    await s.compiled("greeter_impl.cpp");
    await s.indexed();
    expect(await names(s, "Greeter"), "Index not ready").toContain("Greeter");
    expect(
        into(s, await s.definition(at("greeter_impl.cpp", "module G|reeter;")), "greeter.cppm"),
    ).toEqual(["greeter.cppm: export module Greeter;"]);
});

serve.data("modules/dotted_module_name")("dotted import definition", async ({ s }) => {
    await s.compiled("app.cppm");
    await s.indexed();
    expect(await names(s, "my.io"), "Index not ready").toContain("my.io");
    expect(into(s, await s.definition(at("app.cppm", "import my|.io;")), "io.cppm")).toEqual([
        "io.cppm: export module my.io;",
    ]);
});

serve.data("modules/module_partitions")("partition import definition", async ({ s }) => {
    await s.compiled("lib.cppm");
    await s.indexed();
    expect(await names(s, "Lib:A"), "Index not ready").toContain("Lib:A");
    // The partition name resolves through the enclosing module.
    expect(
        into(s, await s.definition(at("lib.cppm", "export import :|A;")), "part_a.cppm"),
    ).toEqual(["part_a.cppm: export module Lib:A;"]);
});

serve.data("modules/macro_import")("macro import definition", async ({ s }) => {
    // `import MATH_MODULE;` where the name comes from a macro: the index
    // anchors the occurrence at the expansion site.
    await s.compiled("main.cpp");
    await s.indexed();
    expect(await names(s, "Math"), "Index not ready").toContain("Math");
    expect(
        into(s, await s.definition(at("main.cpp", "import MA|TH_MODULE;")), "math.cppm"),
    ).toEqual(["math.cppm: export module Math;"]);
});
