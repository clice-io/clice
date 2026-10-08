/// clang-tidy on open files: the nearest .clang-tidy's checks minus the
/// editor's exclusions, NOLINT, fixes through code actions, configuration
/// edits, and a crashing check.

import type * as proto from "vscode-languageserver-protocol";
import { MTIME_GRANULARITY, sleep, waitUntil, type CliceClient } from "@clice/tools/client";
import { applyTextEdits, editsFor } from "@clice/tools/client/edits";
import { expect, test } from "../fixtures.ts";

function text(diagnostic: proto.Diagnostic): string {
    return typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value;
}

/// The published diagnostics of `source` as `line:code`.
function published(client: CliceClient, uri: string, source = "clang-tidy"): string[] {
    return (client.diagnostics.get(uri) ?? [])
        .filter((diagnostic) => diagnostic.source === source)
        .map((diagnostic) => `${diagnostic.range.start.line}:${String(diagnostic.code)}`);
}

async function quickFixes(
    client: CliceClient,
    uri: string,
    range: proto.Range,
): Promise<proto.CodeAction[]> {
    const reply = (await client.codeActions(uri, range)) ?? [];
    return reply.filter(
        (item): item is proto.CodeAction => "kind" in item && item.kind === "quickfix",
    );
}

test("findings name their check", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    workspace.write("main.cpp", "double ratio(int a, int b) { return a / b; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const finding = client.diagnostics.get(uri)?.find((d) => d.source === "clang-tidy");
    expect(finding?.code).toBe("bugprone-integer-division");
    expect(finding?.codeDescription?.href).toBe(
        "https://clang.llvm.org/extra/clang-tidy/checks/bugprone/integer-division.html",
    );
    expect(text(finding!)).not.toContain("[bugprone-integer-division]");
    expect(finding?.range.start).toEqual({ line: 0, character: 36 });
});

test("defaults apply without configuration", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "namespace n {}\nnamespace alias = n;\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    expect(published(client, uri)).toEqual(["1:misc-unused-alias-decls"]);
    const [finding] = client.diagnostics.get(uri)!;
    expect(finding?.tags).toEqual([1]);
});

test("unusable and slow checks stay off", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        ".clang-tidy",
        'Checks: "-*,bugprone-integer-division,bugprone-use-after-move,misc-const-correctness"\n',
    );
    workspace.write(
        "main.cpp",
        "namespace std {\n" +
            "template <class T> T&& move(T& t) { return static_cast<T&&>(t); }\n" +
            "}\n" +
            "struct S { int v; };\n" +
            "int use(S s) { S t = std::move(s); return s.v + t.v; }\n" +
            "double ratio(int a, int b) { return a / b; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    expect(published(client, uri)).toEqual(["5:bugprone-integer-division"]);
});

test("NOLINT silences findings and warnings", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    workspace.write(
        "main.cpp",
        "double r1(int a, int b) { return a / b; } // NOLINT\n" +
            "// NOLINTNEXTLINE(bugprone-integer-division)\n" +
            "double r2(int a, int b) { return a / b; }\n" +
            "double r3(int a, int b) { return a / b; }\n" +
            "int f() { int unused; return 0; } // NOLINT\n" +
            "int g() { int unused; return 0; }\n",
    );
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-Wunused-variable"] });
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    expect(published(client, uri)).toEqual(["3:bugprone-integer-division"]);
    expect(published(client, uri, "clang")).toEqual(["5:warn_unused_variable"]);
});

test("a fix applies through a code action", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    workspace.write("main.cpp", "int* p = 0;\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri, content] = await client.openAndWait("main.cpp");

    const [finding] = client.diagnostics.get(uri)!;
    expect(finding?.code).toBe("modernize-use-nullptr");
    const [fix] = await quickFixes(client, uri, finding!.range);
    expect(fix?.title).toBe("change '0' to 'nullptr'");
    const fixed = applyTextEdits(content, editsFor(fix!, uri));
    expect(fixed).toBe("int* p = nullptr;\n");

    client.change(uri, 1, fixed);
    await client.waitForRecompile(uri);
    expect(published(client, uri)).toEqual([]);
});

test("preamble includes reach the checks", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(".clang-tidy", 'Checks: "-*,readability-duplicate-include"\n');
    workspace.write("a.h", "#pragma once\n");
    workspace.write(
        "main.cpp",
        '#if 0\n#include "a.h"\n#endif\n#include "a.h"\nint x;\n#include "a.h"\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri, content] = await client.openAndWait("main.cpp");

    // The include past the preamble repeats the preamble's active one;
    // underlined as the fix removes it, the newline ending line 4 first.
    expect(published(client, uri)).toEqual(["4:readability-duplicate-include"]);
    const [finding] = client.diagnostics.get(uri)!;
    expect(finding?.range.end).toEqual({ line: 5, character: 14 });
    const [fix] = await quickFixes(client, uri, finding!.range);
    expect(applyTextEdits(content, editsFor(fix!, uri))).toBe(
        '#if 0\n#include "a.h"\n#endif\n#include "a.h"\nint x;\n',
    );
});

test("an inserted include the preamble has is left out", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        ".clang-tidy",
        'Checks: "-*,modernize-make-unique"\n' +
            "CheckOptions:\n" +
            "  modernize-make-unique.MakeSmartPtrFunction: 'mem::make_unique'\n" +
            "  modernize-make-unique.MakeSmartPtrFunctionHeader: 'mem.h'\n",
    );
    workspace.write(
        "mem.h",
        "#pragma once\n" +
            "namespace std {\n" +
            "template <class T> struct default_delete {};\n" +
            "template <class T, class D = default_delete<T>> struct unique_ptr {\n" +
            "    explicit unique_ptr(T*);\n" +
            "    ~unique_ptr();\n" +
            "};\n" +
            "}\n",
    );
    workspace.write(
        "main.cpp",
        '#include "mem.h"\nstd::unique_ptr<int> make() { return std::unique_ptr<int>(new int(1)); }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri, content] = await client.openAndWait("main.cpp");

    const [finding] = client.diagnostics.get(uri)!;
    expect(finding?.code).toBe("modernize-make-unique");
    const [fix] = await quickFixes(client, uri, finding!.range);
    expect(applyTextEdits(content, editsFor(fix!, uri))).toBe(
        '#include "mem.h"\nstd::unique_ptr<int> make() { return mem::make_unique<int>(1); }\n',
    );
});

test("configuration edits are picked up", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/main.cpp", "int* p = 0;\n");
    workspace.writeCDB(["src/main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("src/main.cpp");
    expect(published(client, uri)).toEqual([]);

    // Created where the lookup found none, then edited.
    workspace.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    await client.poll("workspace");
    await client.waitForRecompile(uri);
    expect(published(client, uri)).toEqual(["0:modernize-use-nullptr"]);

    await sleep(MTIME_GRANULARITY);
    workspace.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    await client.poll("workspace");
    await client.waitForRecompile(uri);
    expect(published(client, uri)).toEqual([]);
});

test("off by configuration", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("clice.toml", "[diagnostics]\nclang_tidy = false\n");
    workspace.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    workspace.write("main.cpp", "double ratio(int a, int b) { return a / b; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    expect(published(client, uri)).toEqual([]);
});

/// The worker crashes are the point here: the session opts out of the
/// anomaly gate, and Debug builds must not trap on them.
function crashing(env: Record<string, string>) {
    return { allowAnomaly: true, env: { CLICE_ANOMALY_NO_TRAP: "1", ...env } };
}

/// The crash notes on the file, once one has landed.
async function crashNotes(client: CliceClient, uri: string): Promise<string[]> {
    const notes = () =>
        (client.diagnostics.get(uri) ?? [])
            .map(text)
            .filter((message) => message.includes("clice's worker crashed"));
    await waitUntil(() => notes().length > 0, {
        timeout: 20_000,
        interval: 200,
        description: "a crash note",
    });
    return notes();
}

test("a crashing check pauses only clang-tidy", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    workspace.write(
        "main.cpp",
        "// tidy poison\ndouble ratio(int a, int b) { return a / b; }\nint broken = nullptr;\n",
    );
    workspace.writeCDB(["main.cpp"]);
    const client = session.spawn(workspace, crashing({ CLICE_TEST_TIDY_CRASH: "tidy poison" }));
    await client.initialize(workspace);
    const [uri] = client.open("main.cpp");
    expect(await client.hoverAt(uri, 1, 8)).not.toBeNull();

    expect(await crashNotes(client, uri)).toEqual([
        expect.stringContaining("while running clang-tidy on this file"),
    ]);
    expect(published(client, uri)).toEqual([]);
    expect(published(client, uri, "clang")).toEqual(["2:err_init_conversion_failed"]);
    expect(workspace.workerCrashes(`compile ${workspace.displayPath("main.cpp")}`)).toBe(1);
});

test("a compile crash acquits clang-tidy", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    workspace.write(
        "main.cpp",
        "int add(int a, int b) { return a + b; }\n#pragma clang __debug crash\n",
    );
    workspace.writeCDB(["main.cpp"]);
    const client = session.spawn(workspace, crashing({ CLICE_TEST_PRAGMA_CRASH: "1" }));
    await client.initialize(workspace);
    const [uri] = client.open("main.cpp");
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();

    expect(await crashNotes(client, uri)).toEqual([
        expect.stringContaining("while compiling this file"),
    ]);
    const compile = `compile ${workspace.displayPath("main.cpp")}`;
    const crashes = async (count: number) => {
        await waitUntil(() => workspace.workerCrashes(compile) >= count, {
            timeout: 20_000,
            interval: 100,
            description: `${count} crashes logged`,
        });
        expect(workspace.workerCrashes(compile)).toBe(count);
    };
    await crashes(2);

    // The retry a save grants runs without the pass it acquitted.
    client.save(uri);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await crashes(3);
});
