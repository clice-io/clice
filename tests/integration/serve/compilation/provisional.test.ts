/// Provisional members: a source the user saves that no database lists
/// joins the build under a nearby unit's command — it provides its module,
/// hosts the headers it includes and is indexed — until a database lists it.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import type { Manifest } from "@clice/tools/project";
import { at, expect, serve } from "../../fixtures.ts";

const A = "export module a;\nexport int fa() { return 1; }\n";
const B = "export module b;\nexport int fb() { return 2; }\nint b_local() { return 3; }\n";
const IMPORTS_A = "import a;\nint main() { return fa(); }\n";
const IMPORTS_AB = "import a;\nimport b;\nint main() { return fa() + fb(); }\n";
const PART = "#ifndef UNITY\n#error needs the unity file\n#endif\nint part() { return 0; }\n";
const UNITY = '#define UNITY 1\n#include "part.cpp"\n';

/// Write a file the way an editor creates one: on disk, opened, saved.
/// Returns the errors it opened with.
async function saveNew(s: Serve, file: string, text: string): Promise<proto.Diagnostic[]> {
    s.disk.write(file, text);
    const errors = await s.errors(file);
    s.save(file);
    return errors;
}

function cxx20(...units: string[]): Manifest {
    return { cxx: ["-std=c++20"], units: Object.fromEntries(units.map((unit) => [unit, []])) };
}

/// The database rewritten to `manifest`, and reloaded.
async function reload(s: Serve, manifest: Manifest): Promise<void> {
    s.disk.database(manifest);
    expect(await s.poll("cdb", { force: true }), "the reload changes the build").toBe(1);
}

/// The index's definitions named `name` exactly.
async function defined(s: Serve, name: string): Promise<string> {
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    return s.show(symbols.filter((symbol) => symbol.name === name));
}

const modules = serve.files(
    { "a.cppm": A, "main.cpp": IMPORTS_A },
    { manifest: cxx20("a.cppm", "main.cpp") },
);

modules("saved module is imported", async ({ s }) => {
    await s.compiled("main.cpp");
    await saveNew(s, "b.cppm", B);
    s.edit("main.cpp", { text: IMPORTS_AB });
    expect(await s.errors("main.cpp"), "the saved module provides b").toEqual([]);
});

modules("saved module survives restart", async ({ s }) => {
    await saveNew(s, "b.cppm", B);
    await s.offline(() => {
        s.disk.write("main.cpp", IMPORTS_AB);
    });
    expect(await s.errors("main.cpp"), "the record outlives the session").toEqual([]);
    await s.indexed();
    expect(await defined(s, "b_local"), "the restored member is indexed").toBe(
        "b_local b.cppm: int b_local() { return 3; }",
    );
});

serve.files(
    {
        "a.cppm": "export module a;\nexport import :part;\nexport int fa() { return 1; }\n",
        "main.cpp": "import a;\nint main() { return fa() + fpart(); }\n",
    },
    { manifest: cxx20("a.cppm", "main.cpp") },
)("saved partition joins module", async ({ s }) => {
    expect(await s.errors("main.cpp"), "no file provides a:part yet").not.toEqual([]);
    await saveNew(s, "a-part.cppm", "export module a:part;\nexport int fpart() { return 3; }\n");
    expect(await s.errors("main.cpp"), "the partition completes the module").toEqual([]);
});

serve.files({ "a.cppm": A, "main.cpp": IMPORTS_AB }, { manifest: cxx20("a.cppm", "main.cpp") })(
    "deleted member comes back",
    async ({ s }) => {
        await saveNew(s, "b.cppm", B);
        expect(await s.errors("main.cpp")).toEqual([]);

        s.disk.rm("b.cppm");
        await s.sync({ poll: true });
        expect(await s.errors("main.cpp"), "a deleted module provides nothing").not.toEqual([]);

        // The reload rebuilds the graph from the build's units: only the kept
        // record still counts the missing file among them.
        s.disk.write("c.cpp", "int c() { return 0; }\n");
        await reload(s, cxx20("a.cppm", "main.cpp", "c.cpp"));

        s.disk.write("b.cppm", B);
        await s.sync({ poll: true });
        expect(await s.errors("main.cpp"), "the record brings it back with the file").toEqual([]);
    },
);

serve.files({
    "src/shared.h": "#pragma once\nint shared_fn();\n",
    "src/a.cpp": '#include "shared.h"\nint shared_fn() { return 0; }\n',
})("closed member stays indexed", async ({ s }) => {
    await s.compiled("src/a.cpp");
    await saveNew(
        s,
        "src/new.cpp",
        '#include "shared.h"\nint fresh_fn() { return shared_fn(); }\n',
    );
    s.close("src/new.cpp");
    await s.indexed();
    expect(await defined(s, "fresh_fn"), "the member is indexed").toBe(
        "fresh_fn src/new.cpp: int fresh_fn() { return shared_fn(); }",
    );
    expect(s.show(await s.references(at("src/a.cpp", "int s|hared_fn() {")))).toContain(
        "src/new.cpp: int fresh_fn() { return shared_fn(); }",
    );
});

serve.files(
    { "src/a.cpp": "int a() { return 0; }\n", "src/s.cpp": "int s() { return 0; }\n" },
    { config: { project: { idle_timeout_ms: 10 } } },
)("database takes over, then drops", async ({ s }) => {
    const listed = s.manifest;
    await s.compiled("src/a.cpp");
    await saveNew(
        s,
        "src/new.cpp",
        "#ifndef LISTED\nint regen_fn() { return 1; }\n#else\nint listed_fn() { return 2; }\n#endif\n",
    );
    s.close("src/new.cpp");
    await s.indexed();
    expect(await defined(s, "regen_fn")).toBe("regen_fn src/new.cpp: int regen_fn() { return 1; }");

    await reload(s, { ...listed, units: { ...listed.units, "src/new.cpp": ["-DLISTED"] } });
    await s.indexed();
    expect(await defined(s, "listed_fn"), "the entry's command takes over").toBe(
        "listed_fn src/new.cpp: int listed_fn() { return 2; }",
    );
    expect(await defined(s, "regen_fn")).toBe("");

    await reload(s, listed);
    await s.indexed();
    expect(await defined(s, "listed_fn"), "the file leaving the index with its entry").toBe("");
    // Queued after the reload: a record outliving the takeover would have
    // the file indexed again by the time this lands.
    s.disk.write("src/s.cpp", "int s() { return 0; }\nint sentinel_fn() { return 3; }\n");
    await s.sync({ poll: true });
    expect(await defined(s, "sentinel_fn")).toBe(
        "sentinel_fn src/s.cpp: int sentinel_fn() { return 3; }",
    );
    expect(await defined(s, "regen_fn"), "no record brings it back").toBe("");

    await s.compiled("src/new.cpp");
    s.save("src/new.cpp");
    s.close("src/new.cpp");
    await s.indexed();
    expect(await defined(s, "regen_fn"), "a save records it again").toBe(
        "regen_fn src/new.cpp: int regen_fn() { return 1; }",
    );
});

serve.files(
    {
        "src/a.cpp": "int a() { return 0; }\n",
        "tools/legacy.c": "int legacy(void) { return 0; }\n",
    },
    { manifest: { units: { "src/a.cpp": [], "tools/legacy.c": [] } } },
)("lost lender drops member", async ({ s }) => {
    await s.compiled("src/a.cpp");
    await saveNew(s, "tools/tool.c", "int tool_fn(void) { return 1; }\n");
    s.close("tools/tool.c");
    await s.indexed();
    expect(await defined(s, "tool_fn")).toBe(
        "tool_fn tools/tool.c: int tool_fn(void) { return 1; }",
    );

    await reload(s, { units: { "src/a.cpp": [] } });
    await s.indexed();
    expect(
        await defined(s, "tool_fn"),
        "a .c with no C unit left to borrow from leaving the index",
    ).toBe("");
});

serve.files({ "lib/x.cpp": "int x() { return 0; }\n", "lib/frag.h": "Ctx use();\n" })(
    "member hosts its headers",
    async ({ s }) => {
        expect(await s.errors("lib/frag.h"), "nothing provides Ctx yet").not.toEqual([]);
        await saveNew(s, "lib/new.cpp", 'struct Ctx {};\n#include "frag.h"\n');
        expect(await s.errors("lib/frag.h"), "the member lends its context to the header").toEqual(
            [],
        );
    },
);

serve.files({ "unity.cpp": UNITY })("included source keeps host", async ({ s }) => {
    expect(await s.errors("unity.cpp"), "part.cpp does not exist yet").not.toEqual([]);
    await saveNew(s, "part.cpp", PART);
    s.edit("part.cpp", { text: PART + "int more() { return 1; }\n" });
    expect(await s.errors("part.cpp"), "a source a unit includes is no member of its own").toEqual(
        [],
    );
    await s.stop();

    await s.start();
    expect(await s.errors("part.cpp"), "nor after a restart").toEqual([]);
});

serve.files({ "other.cpp": "int other() { return 0; }\n" })(
    "regenerated unity takes member",
    async ({ s }) => {
        expect(
            await saveNew(s, "part.cpp", PART),
            "a member of its own, without the unity file",
        ).not.toEqual([]);
        s.close("part.cpp");

        s.disk.write("unity.cpp", UNITY);
        await reload(s, { ...s.manifest, units: { "other.cpp": [], "unity.cpp": [] } });
        expect(await s.errors("part.cpp"), "the regenerated unity file hosts it").toEqual([]);
    },
);

serve.files({
    "src/a.cpp": "int a() { return 0; }\n",
    "out/CMakeCache.txt": "",
    "clice.toml": '[project]\ncache_dir = "${workspace}/.clice"\n',
})("query labels the members", async ({ s }) => {
    await saveNew(s, "src/new.cpp", "int fresh_fn() { return 1; }\n");
    await saveNew(s, "src/orphan.h", "#pragma once\n");
    await saveNew(s, "out/gen.cpp", "int gen() { return 0; }\n");
    s.disk.write("src/opened.cpp", "int opened() { return 0; }\n");
    await s.compiled("src/opened.cpp");
    await s.stop();

    // What `clice query compileCommand` says a file's command came from.
    const source = async (file: string) =>
        (
            (await s.cli("query", "--method", "compileCommand", "--path", file)).json as {
                result?: { source: string };
            }
        ).result?.source;
    expect(await source("src/new.cpp")).toBe("provisional");
    expect(await source("src/orphan.h"), "a header is never one").toBe("inferred");
    expect(await source("out/gen.cpp"), "a build tree holds none").toBe("inferred");
    expect(await source("src/opened.cpp"), "opening records nothing").toBe("inferred");
});

serve.files(
    {
        "include/util.h":
            "#pragma once\n#ifndef FROM_A\n#error needs FROM_A\n#endif\ninline int util() { return 1; }\n",
        "src/a.cpp": "int a() { return 0; }\n",
    },
    {
        manifest: {
            cxx: ["-std=c++20"],
            units: { "src/a.cpp": ["-DFROM_A", "-I${workspace}/include"] },
        },
    },
)("first module borrows project flags", async ({ s }) => {
    expect(
        await saveNew(
            s,
            "src/first.cppm",
            'module;\n#include "util.h"\nexport module first;\nexport int f() { return util(); }\n',
        ),
        "the module compiles under the .cpp unit's flags",
    ).toEqual([]);
    await s.compiled("src/a.cpp");
    s.edit("src/a.cpp", { text: "import first;\nint a() { return f(); }\n" });
    expect(await s.errors("src/a.cpp"), "the database unit imports it").toEqual([]);
});

const IMPL =
    "module m;\n#ifndef FROM_MODS\n#error needs the interface's flags\n#endif\nint mv() { return 1; }\n";

serve.files(
    {
        "src/main.cpp": "int main() { return 0; }\n",
        "mods/m.cppm": "export module m;\nexport int mv();\n",
    },
    {
        manifest: {
            cxx: ["-std=c++20"],
            units: { "src/main.cpp": [], "mods/m.cppm": ["-DFROM_MODS"] },
        },
    },
)("implementation unit borrows interface", async ({ s }) => {
    expect(await saveNew(s, "mods/m_impl.cpp", IMPL), "the interface next to it lends").toEqual([]);
    s.edit("mods/m_impl.cpp", { text: IMPL + "int twice() { return mv() + mv(); }\n" });
    expect(await s.errors("mods/m_impl.cpp"), "and keeps lending once it is a member").toEqual([]);
});
