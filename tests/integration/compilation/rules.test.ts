/// Rules as the source of compile commands: declared databases, default
/// commands, anchored patterns, and the edits a header inherits from its
/// host. Every workspace is a tmpdir with its own clice.toml.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

function gated(macro: string): string {
    return `#ifndef ${macro}\n#error missing ${macro}\n#endif\nint main() { return 0; }\n`;
}

function guidance(client: CliceClient, uri: string): number {
    return (client.diagnostics.get(uri) ?? []).filter((d) => d.code === "inferred-compile-command")
        .length;
}

test("default command compiles files", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("clice.toml", '[[rules]]\ndefault_command = "clang++ -std=c++20 -DFEATURE"\n');
    workspace.write("main.cpp", gated("FEATURE"));
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    client.assertNoErrors(uri, "the declared default command defines FEATURE");
    expect(guidance(client, uri), "a declared command is not a guess").toBe(0);

    // A file created after startup takes the same command at once.
    workspace.write("later.cpp", gated("FEATURE"));
    const [later] = await client.openAndWait("later.cpp");
    client.assertNoErrors(later, "new files compile under the default command");
});

test("declared source ignores discovered database", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", gated("FROM_RULE"));
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-DFROM_CDB"] });
    workspace.write(
        "clice.toml",
        '[[rules]]\ndefault_command = ["clang++", "-std=c++20", "-DFROM_RULE"]\n',
    );
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    client.assertNoErrors(uri, "the declared command wins over the database at the root");
});

test("databases load in declared order", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", gated("FROM_A"));
    workspace.write("other.cpp", gated("FROM_B"));
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-DFROM_A"], at: "a/compile_commands.json" });
    workspace.writeEntries(
        [
            ["main.cpp", ["-DFROM_B"]],
            ["other.cpp", ["-DFROM_B"]],
        ],
        { at: "b/compile_commands.json" },
    );
    workspace.write("clice.toml", '[[rules]]\ncompile_commands = ["a", "b"]\n');
    await client.initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    client.assertNoErrors(main, "the first database wins for a file both list");
    const [other] = await client.openAndWait("other.cpp");
    client.assertNoErrors(other, "the second database fills in what the first lacks");
    const contexts = await client.queryContext(workspace.uri("main.cpp"));
    expect(contexts.total, "both entries stay switchable").toBe(2);
});

test("rule binds a subtree to its database", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/a.cpp", gated("ROOT"));
    workspace.write("lib/x.cpp", gated("LIB"));
    workspace.writeEntries(
        [
            ["src/a.cpp", ["-DROOT"]],
            ["lib/x.cpp", ["-DROOT"]],
        ],
        { at: "cmake/compile_commands.json" },
    );
    workspace.writeCDB(["lib/x.cpp"], {
        extraArgs: ["-DLIB"],
        at: "lib/cmake/compile_commands.json",
    });
    workspace.write(
        "clice.toml",
        '[[rules]]\npatterns = ["lib/**"]\ncompile_commands = ["lib/cmake"]\n\n[[rules]]\ncompile_commands = ["cmake"]\n',
    );
    await client.initialize(workspace);

    const [a] = await client.openAndWait("src/a.cpp");
    client.assertNoErrors(a, "src/ keeps the workspace database");
    const [x] = await client.openAndWait("lib/x.cpp");
    client.assertNoErrors(x, "lib/ takes the rule's database first");
});

test("relative patterns anchor at the config", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/main.cpp", gated("FEATURE"));
    workspace.write("other.cpp", gated("FEATURE"));
    workspace.writeCDB(["src/main.cpp", "other.cpp"]);
    workspace.write("clice.toml", '[[rules]]\npatterns = ["src/**"]\nappend = ["-DFEATURE"]\n');
    await client.initialize(workspace);

    const [main] = await client.openAndWait("src/main.cpp");
    client.assertNoErrors(main, "src/** matches the file under the config directory");
    const [other] = await client.openAndWait("other.cpp");
    client.assertHasErrors(other, "the pattern does not reach outside src/");
});

test("header inherits host edits", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/main.cpp", '#include "../include/x.h"\nint main() { return x(); }\n');
    workspace.write(
        "include/x.h",
        "#ifndef BUILDING_LIB\n#error missing BUILDING_LIB\n#endif\ninline int x() { return 0; }\n",
    );
    workspace.writeCDB(["src/main.cpp"]);
    workspace.write(
        "clice.toml",
        '[[rules]]\npatterns = ["src/**"]\nappend = ["-DBUILDING_LIB"]\n',
    );
    await client.initialize(workspace);

    const [header] = await client.openAndWait("include/x.h");
    client.assertNoErrors(header, "the header compiles with the macro its host's rule adds");
});

test("header borrows default command host", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/main.cpp", '#include "../include/x.h"\nint main() { return x(); }\n');
    workspace.write(
        "include/x.h",
        "#ifndef FROM_HOST\n#error missing FROM_HOST\n#endif\ninline int x() { return 0; }\n",
    );
    workspace.write(
        "clice.toml",
        '[[rules]]\npatterns = ["src/**"]\ndefault_command = "clang++ -std=c++20 -DFROM_HOST"\n',
    );
    await client.initialize(workspace);

    const [header] = await client.openAndWait("include/x.h");
    client.assertNoErrors(header, "the header compiles under its host's default command");
    const contexts = await client.queryContext(workspace.uri("include/x.h"));
    expect(contexts.total, "the default-command host is offered as a context").toBe(1);
});

test("index skips excluded units", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cpp",
        "int main_sym() { return 0; }\nint main() { return main_sym(); }\n",
    );
    workspace.write("third_party/lib.cpp", "int tp_sym() { return 1; }\n");
    workspace.writeCDB(["main.cpp", "third_party/lib.cpp"]);
    workspace.write("clice.toml", '[[rules]]\npatterns = ["third_party/**"]\nindex = false\n');
    await client.initialize(workspace);

    const [main] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(main, "main_sym")).toBe(true);
    const symbols = await client.workspaceSymbols("tp_sym");
    expect(symbols?.length ?? 0, "an excluded unit contributes no symbols").toBe(0);

    // A command change re-enqueues the unit through the reload path; the
    // exclusion holds there as well.
    workspace.writeCDB(["main.cpp", "third_party/lib.cpp"], { extraArgs: ["-DCHANGED"] });
    expect((await client.poll("cdb")).events).toBe(1);
    await client.waitForRecompile(main);
    expect(await client.waitForIndex(main, "main_sym")).toBe(true);
    const after = await client.workspaceSymbols("tp_sym");
    expect(after?.length ?? 0, "a reloaded excluded unit stays out of the index").toBe(0);
});

test("excluded unit escalates when read", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.write("third_party/lib.cpp", "int tp_sym() { return 1; }\n");
    workspace.writeCDB(["main.cpp", "third_party/lib.cpp"]);
    workspace.write("clice.toml", '[[rules]]\npatterns = ["third_party/**"]\nindex = false\n');
    await client.initialize(workspace, {
        initializationOptions: { project: { readonly: "auto" } },
    });

    // No shard will ever serve an excluded unit: the didOpen boost is
    // refused and the session escalates to a pulled compile at once.
    const uri = workspace.uri("third_party/lib.cpp");
    const arrived = client.armDiagnostics(uri);
    client.open("third_party/lib.cpp");
    const symbols = (await client.documentSymbols(uri)) as proto.DocumentSymbol[] | null;
    expect(symbols?.map((s) => s.name)).toContain("tp_sym");
    await arrived;
    client.assertNoErrors(uri);
});
