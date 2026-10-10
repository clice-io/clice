/// Integration tests for C++20 module support.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { MTIME_GRANULARITY } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

/// Loose files built as C++20 units, each with its own arguments.
function cxx20(units: Record<string, string[]>) {
    return { manifest: { cxx: ["-std=c++20"], units } };
}

/// The module names the index lists for `query`.
async function names(s: Serve, query: string): Promise<string[]> {
    return ((await s.workspaceSymbols(query)) ?? [])
        .filter((symbol) => symbol.kind === proto.SymbolKind.Module)
        .map((symbol) => symbol.name);
}

/// The lines of a rendered reply that point into `file`.
function into(s: Serve, reply: unknown, file: string): string[] {
    return s
        .show(reply)
        .split("\n")
        .filter((line) => line.startsWith(`${file}: `));
}

/// The units of each module topology, in the order the case compiles them
/// clean: the first one opened builds the modules it imports on demand.
const TOPOLOGIES = {
    "modules/single_module_no_deps": ["mod_a.cppm"],
    "modules/chained_modules": ["mod_b.cppm"],
    "modules/diamond_modules": ["top.cppm"],
    "modules/dotted_module_name": ["app.cppm"],
    // An implementation unit (`module M;` without export) compiles using
    // the interface PCM.
    "modules/module_implementation_unit": ["greeter_impl.cpp"],
    // Partitions are compiled in dependency order.
    "modules/module_partitions": ["lib.cppm"],
    "modules/partition_interface": ["primary.cppm"],
    "modules/partition_chain": ["sys.cppm"],
    // Internal partitions (`module M:part;`) import other partitions and
    // are imported by the other units of M, among them an implementation
    // unit, which imports the primary interface implicitly.
    "modules/internal_partitions": [
        "detail.cppm",
        "util.cppm",
        "api.cppm",
        "lib.cppm",
        "impl.cpp",
        "main.cpp",
    ],
    // Re-exported symbols (export import) are accessible through the wrapper.
    "modules/re_export": ["user.cppm"],
    "modules/global_module_fragment": ["gmf.cppm"],
    "modules/gmf_with_import": ["combined.cppm"],
    "modules/independent_modules": ["x.cppm", "y.cppm"],
    "modules/deep_chain": ["m5.cppm"],
    "modules/partition_with_gmf": ["cfg.cppm"],
    "modules/partition_with_external_import": ["app.cppm"],
};

serve.each(Object.keys(TOPOLOGIES))("compiles clean", async ({ s }) => {
    for (const file of TOPOLOGIES[s.project as keyof typeof TOPOLOGIES]) {
        await s.clean(file);
    }
});

/// Circular module imports should not hang the server.
///
/// The CompileGraph's cycle detection should prevent deadlock. We verify
/// the server settles and stays responsive by opening a non-cyclic file
/// afterwards.
serve("modules/circular_module_dependency")("circular module dependency", async ({ s }) => {
    s.open("cycle_a.cppm");
    await s.sync();
    await s.clean("ok.cppm");
});

/// A regular .cpp that imports a module should get PCM deps compiled first.
serve("shapes/modules")("consumer imports module", async ({ s }) => {
    await s.clean(s.file("main"));
});

serve("shapes/modules")("internal partition definition", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    await s.indexed();
    expect(await names(s, "shapes:detail"), "Index not ready").toContain("shapes:detail");
    expect(
        into(
            s,
            await s.definition(at(s.file("circle_impl"), "import :|detail;")),
            s.file("detail"),
        ),
    ).toEqual([`${s.file("detail")}: module shapes:detail;`]);
});

/// An overload of `area` the importer's call on a non-const Circle
/// prefers: only a reindex of the importer against the new interface
/// points the call at it. The case inserts it, so the sample lacks it.
const PREFERRED = "area(Circle& circle)";

serve("shapes/modules")("module save reindexes importers", async ({ s }) => {
    const call = `${s.file("main")}: double total = shapes::area(c) + triangle.measure() + shapes_circle_area(1.0);`;
    await s.compiled(s.file("circle"));
    await s.indexed();
    expect(
        into(
            s,
            await s.references(at(s.file("circle"), "double |area(const Circle&")),
            s.file("main"),
        ),
        "initial index never produced the importer's area(const Circle&) reference",
    ).toEqual([call]);

    // The next server's startup sweep finds the importer fresh and never
    // runs it: only the index's record of what it imported leads the save
    // to it.
    await s.stop();
    await s.start({ config: { project: { idle_timeout_ms: 10 } } });
    await s.compiled(s.file("circle"));
    s.edit(s.file("circle"), {
        after: "double area(const Circle& circle);",
        insert: `\n\ndouble ${PREFERRED};`,
    });
    // The rewrite must stat newer than the text the importer was indexed against.
    s.disk.edit(s.file("circle"), {
        after: "double area(const Circle& circle);",
        insert: `\n\ndouble ${PREFERRED};`,
    });
    s.disk.touch(
        s.file("circle"),
        new Date(s.disk.mtime(s.file("circle")).getTime() + MTIME_GRANULARITY),
    );
    s.save(s.file("circle"), { write: false });
    await s.sync();

    expect(
        into(s, await s.references(at(s.file("circle"), PREFERRED)), s.file("main")),
        "the importer was not reindexed after the module save",
    ).toEqual([call]);
    expect(
        into(
            s,
            await s.references(at(s.file("circle"), "double |area(const Circle&")),
            s.file("main"),
        ),
    ).toEqual([]);
});

serve.files(
    {
        "block.cppm":
            "export module Block;\nexport {\nint alpha() { return 1; }\nint beta() { return 2; }\nnamespace ns {\nint gamma() { return 3; }\n}\n}\n",
        "consumer.cppm":
            "export module Consumer;\nimport Block;\nexport int total() { return alpha() + beta() + ns::gamma(); }\n",
    },
    cxx20({ "block.cppm": [], "consumer.cppm": [] }),
)("export block", async ({ s }) => {
    await s.clean("consumer.cppm");
});

serve.files(
    {
        "priv.cppm":
            "export module Priv;\nexport int public_fn();\nmodule : private;\nint public_fn() { return 42; }\nint private_helper() { return 7; }\n",
    },
    cxx20({ "priv.cppm": [] }),
)("private module fragment", async ({ s }) => {
    await s.clean("priv.cppm");
});

serve.files(
    {
        "ns.cppm":
            "export module NS;\nexport namespace math {\nint add(int a, int b) { return a + b; }\nint mul(int a, int b) { return a * b; }\n}\n",
        "calc.cppm":
            "export module Calc;\nimport NS;\nexport int compute() { return math::add(3, math::mul(4, 5)); }\n",
    },
    cxx20({ "ns.cppm": [], "calc.cppm": [] }),
)("export namespace", async ({ s }) => {
    await s.clean("calc.cppm");
});

serve.files(
    {
        "tmpl.cppm":
            "export module Tmpl;\nexport template <typename T>\nT identity(T x) { return x; }\nexport template <typename T, typename U>\nauto pair_sum(T a, U b) { return a + b; }\n",
        "use_tmpl.cppm":
            "export module UseTmpl;\nimport Tmpl;\nexport int test() { return identity(42) + pair_sum(1, 2); }\n",
    },
    cxx20({ "tmpl.cppm": [], "use_tmpl.cppm": [] }),
)("template export", async ({ s }) => {
    await s.clean("use_tmpl.cppm");
});

serve("shapes/modules")("class export and inheritance", async ({ s }) => {
    await s.clean(s.file("circle"));
});

/// Closing and reopening a modified module file should recompile without errors.
serve("shapes/modules")("save recompile", async ({ s }) => {
    // Open and compile the circle partition (which triggers the shape
    // partition's PCM build).
    await s.clean(s.file("circle"));
    await s.compiled(s.file("shape"));

    // Close the shape partition, modify it on disk, and reopen it with the
    // new content.
    s.close(s.file("shape"));
    s.disk.edit(s.file("shape"), {
        after: "virtual const char* name() const = 0;",
        insert: "\n\n    virtual int corners() const {\n        return 0;\n    }",
    });
    await s.clean(s.file("shape"));
});

serve.files(
    {
        "good.cppm": "export module Good;\nexport int good() { return 1; }\n",
        "bad.cppm":
            "export module Bad;\nimport Good;\n\nexport int bad() {\n    return UNDEFINED_SYMBOL;\n}\n",
    },
    cxx20({ "good.cppm": [], "bad.cppm": [] }),
)("module compile error", async ({ s }) => {
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

/// Hover on a symbol imported from a module should return type info.
serve("shapes/modules")("hover on imported symbol", async ({ s }) => {
    await s.clean(s.file("main"));
    const hover = await s.hover(at(s.file("main"), "shapes::|area(c)"));
    expect(hover, "Hover on imported symbol should return info").not.toBeNull();
    expect(hover!.contents).not.toBeNull();
});

/// Plain .cpp with no modules should compile normally (CompileGraph null path).
serve("tiny")("no modules plain cpp", async ({ s }) => {
    await s.clean("main.cpp");
});

serve("shapes/modules")("import definition", async ({ s }) => {
    await s.compiled(s.file("main"));
    await s.indexed();
    expect(await names(s, "shapes"), "Index not ready").toContain("shapes");
    expect(
        into(s, await s.definition(at(s.file("main"), "import s|hapes;")), s.file("library")),
    ).toEqual([`${s.file("library")}: export module shapes;`]);

    // Cursor on the `import` keyword itself must not navigate to the module.
    expect(
        into(s, await s.definition(at(s.file("main"), "im|port shapes;")), s.file("library")),
    ).toEqual([]);
});

serve("shapes/modules")("module decl definition", async ({ s }) => {
    // `module shapes;` in the implementation unit navigates to the interface.
    await s.compiled(s.file("circle_impl"));
    await s.indexed();
    expect(await names(s, "shapes"), "Index not ready").toContain("shapes");
    expect(
        into(
            s,
            await s.definition(at(s.file("circle_impl"), "module s|hapes;")),
            s.file("library"),
        ),
    ).toEqual([`${s.file("library")}: export module shapes;`]);
});

serve("modules/dotted_module_name")("dotted import definition", async ({ s }) => {
    await s.compiled("app.cppm");
    await s.indexed();
    expect(await names(s, "my.io"), "Index not ready").toContain("my.io");
    expect(into(s, await s.definition(at("app.cppm", "import my|.io;")), "io.cppm")).toEqual([
        "io.cppm: export module my.io;",
    ]);
});

serve("shapes/modules")("partition import definition", async ({ s }) => {
    await s.compiled(s.file("library"));
    await s.indexed();
    expect(await names(s, "shapes:shape"), "Index not ready").toContain("shapes:shape");
    // The partition name resolves through the enclosing module.
    expect(
        into(
            s,
            await s.definition(at(s.file("library"), "export import :|shape;")),
            s.file("shape"),
        ),
    ).toEqual([`${s.file("shape")}: export module shapes:shape;`]);
});

serve.files(
    {
        "math.cppm": "export module Math;\nexport int add(int a, int b) { return a + b; }\n",
        "main.cpp":
            "#define MATH_MODULE Math\nimport MATH_MODULE;\n\nint main() {\n    return add(1, 2);\n}\n",
    },
    cxx20({ "math.cppm": [], "main.cpp": [] }),
)("macro import definition", async ({ s }) => {
    // `import MATH_MODULE;` where the name comes from a macro: the index
    // anchors the occurrence at the expansion site.
    await s.compiled("main.cpp");
    await s.indexed();
    expect(await names(s, "Math"), "Index not ready").toContain("Math");
    expect(
        into(s, await s.definition(at("main.cpp", "import MA|TH_MODULE;")), "math.cppm"),
    ).toEqual(["math.cppm: export module Math;"]);
});
