/// Rules as the source of compile commands: declared databases, default
/// commands, anchored patterns, and the edits a header inherits from its
/// host. Every workspace has its own clice.toml.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { writeDatabase } from "@clice/tools/project";
import { expect, serve } from "../../fixtures.ts";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

function errorsOf(diagnostics: proto.Diagnostic[]): proto.Diagnostic[] {
    return diagnostics.filter((d) => d.severity === proto.DiagnosticSeverity.Error);
}

/// A workspace whose databases are the ones `write` puts in place, none at
/// the root unless it writes one: the server restarts on them.
async function databases(s: Serve, write?: () => void): Promise<void> {
    await s.offline(() => {
        s.disk.rm("compile_commands.json");
        write?.();
    });
}

serve.files(
    {
        "clice.toml": '[[rules]]\ndefault_command = "clang++ -std=c++20 -DFEATURE"\n',
        "main.cpp": gated("FEATURE"),
    },
    { manifest: { units: {} } },
)("default command compiles files", async ({ s }) => {
    await databases(s);
    const diagnostics = await s.compiled("main.cpp");
    expect(errorsOf(diagnostics), "the declared default command defines FEATURE").toEqual([]);
    expect(
        diagnostics.filter((d) => d.code === "inferred-compile-command"),
        "a declared command is not a guess",
    ).toEqual([]);

    // A file created after startup takes the same command at once.
    s.disk.write("later.cpp", gated("FEATURE"));
    expect(await s.errors("later.cpp"), "new files compile under the default command").toEqual([]);
});

serve.files(
    {
        "main.cpp": gated("FROM_RULE"),
        "clice.toml": '[[rules]]\ndefault_command = ["clang++", "-std=c++20", "-DFROM_RULE"]\n',
    },
    { manifest: { cxx: ["-std=c++17"], units: { "main.cpp": ["-DFROM_CDB"] } } },
)("declared source ignores discovered database", async ({ s }) => {
    expect(
        await s.errors("main.cpp"),
        "the declared command wins over the database at the root",
    ).toEqual([]);
});

serve.files(
    {
        "main.cpp": gated("FROM_A"),
        "other.cpp": gated("FROM_B"),
        "clice.toml": '[[rules]]\ncompile_commands = ["a", "b"]\n',
    },
    { manifest: { units: {} } },
)("databases load in declared order", async ({ s }) => {
    await databases(s, () => {
        s.workspace.writeCDB(["main.cpp"], {
            extraArgs: ["-DFROM_A"],
            at: "a/compile_commands.json",
        });
        s.workspace.writeEntries(
            [
                ["main.cpp", ["-DFROM_B"]],
                ["other.cpp", ["-DFROM_B"]],
            ],
            { at: "b/compile_commands.json" },
        );
    });
    expect(await s.errors("main.cpp"), "the first database wins for a file both list").toEqual([]);
    expect(
        await s.errors("other.cpp"),
        "the second database fills in what the first lacks",
    ).toEqual([]);
    const contexts = await s.client.queryContext(s.uri("main.cpp"));
    expect(contexts.total, "both entries stay switchable").toBe(2);
});

serve.files(
    {
        "src/a.cpp": gated("ROOT"),
        "lib/x.cpp": gated("LIB"),
        "clice.toml":
            '[[rules]]\npatterns = ["lib/**"]\ncompile_commands = ["lib/cmake"]\n\n[[rules]]\ncompile_commands = ["cmake"]\n',
    },
    { manifest: { units: {} } },
)("rule binds a subtree to its database", async ({ s }) => {
    await databases(s, () => {
        s.workspace.writeEntries(
            [
                ["src/a.cpp", ["-DROOT"]],
                ["lib/x.cpp", ["-DROOT"]],
            ],
            { at: "cmake/compile_commands.json" },
        );
        s.workspace.writeCDB(["lib/x.cpp"], {
            extraArgs: ["-DLIB"],
            at: "lib/cmake/compile_commands.json",
        });
    });
    expect(await s.errors("src/a.cpp"), "src/ keeps the workspace database").toEqual([]);
    expect(await s.errors("lib/x.cpp"), "lib/ takes the rule's database first").toEqual([]);
});

serve.files({
    "src/main.cpp": gated("FEATURE"),
    "other.cpp": gated("FEATURE"),
    "clice.toml": '[[rules]]\npatterns = ["src/**"]\nappend = ["-DFEATURE"]\n',
})("relative patterns anchor at the config", async ({ s }) => {
    expect(
        await s.errors("src/main.cpp"),
        "src/** matches the file under the config directory",
    ).toEqual([]);
    expect(await s.errors("other.cpp"), "the pattern does not reach outside src/").not.toEqual([]);
});

serve.files({
    "src/main.cpp": '#include "../include/x.h"\nint main() { return x(); }\n',
    "include/x.h":
        "#ifndef BUILDING_LIB\n#error missing BUILDING_LIB\n#endif\ninline int x() { return 0; }\n",
    "clice.toml": '[[rules]]\npatterns = ["src/**"]\nappend = ["-DBUILDING_LIB"]\n',
})("header inherits host edits", async ({ s }) => {
    expect(
        await s.errors("include/x.h"),
        "the header compiles with the macro its host's rule adds",
    ).toEqual([]);
});

serve.files(
    {
        "src/main.cpp": '#include "../include/x.h"\nint main() { return x(); }\n',
        "include/x.h":
            "#ifndef FROM_HOST\n#error missing FROM_HOST\n#endif\ninline int x() { return 0; }\n",
        "clice.toml":
            '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++ -std=c++20 -DFROM_HOST"\n',
    },
    { manifest: { units: {} } },
)("header borrows default command host", async ({ s }) => {
    await databases(s);
    expect(
        await s.errors("include/x.h"),
        "the header compiles under its host's default command",
    ).toEqual([]);
    const contexts = await s.client.queryContext(s.uri("include/x.h"));
    expect(contexts.total, "the default-command host is offered as a context").toBe(1);
});

const EXCLUDED = {
    "third_party/lib.cpp": "int tp_sym() { return 1; }\n",
    "clice.toml": '[[rules]]\npatterns = ["third_party/**"]\nindex = false\n',
};

serve.files({
    ...EXCLUDED,
    "main.cpp": "int main_sym() { return 0; }\nint main() { return main_sym(); }\n",
})("index skips excluded units", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();
    expect(s.show(await s.workspaceSymbols("main_sym"))).toBe(
        "main_sym main.cpp: int main_sym() { return 0; }",
    );
    expect(
        (await s.workspaceSymbols("tp_sym")) ?? [],
        "an excluded unit contributes no symbols",
    ).toEqual([]);

    // A command change re-enqueues the unit through the reload path; the
    // exclusion holds there as well.
    writeDatabase(s.workspace, {
        ...s.manifest,
        units: { "main.cpp": ["-DCHANGED"], "third_party/lib.cpp": ["-DCHANGED"] },
    });
    expect((await s.client.poll("cdb")).events).toBe(1);
    await s.diagnostics("main.cpp");
    expect((await s.counts()).files["main.cpp"]?.compile, "main.cpp compiles again").toBe(2);
    await s.indexed();
    expect(s.show(await s.workspaceSymbols("main_sym"))).toBe(
        "main_sym main.cpp: int main_sym() { return 0; }",
    );
    expect(
        (await s.workspaceSymbols("tp_sym")) ?? [],
        "a reloaded excluded unit stays out of the index",
    ).toEqual([]);
    expect((await s.counts()).files["third_party/lib.cpp"]?.index ?? 0).toBe(0);
});

serve.files(
    { ...EXCLUDED, "main.cpp": "int main() { return 0; }\n" },
    { config: { project: { readonly: "auto" } } },
)("excluded unit escalates when read", async ({ s }) => {
    // No shard will ever serve an excluded unit: the didOpen boost is
    // refused and the session escalates to a pulled compile at once. The
    // raw open sends no pull of its own, which would start the compile.
    const uri = s.uri("third_party/lib.cpp");
    const arrived = s.client.armDiagnostics(uri);
    s.client.open("third_party/lib.cpp");
    const symbols = (await s.request("textDocument/documentSymbol", "third_party/lib.cpp")) as
        proto.DocumentSymbol[] | null;
    expect(symbols?.map((symbol) => symbol.name)).toContain("tp_sym");
    await arrived;
    s.client.assertNoErrors(uri);
});
