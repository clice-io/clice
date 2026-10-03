/// `clice query` reads the persisted index straight from disk: answers
/// carry the files whose rows were withheld because the disk moved on,
/// and --fresh brings the index up to date first — through the running
/// server when one holds the writer lock, else by a batch run.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import { basename } from "node:path";
import { waitUntil, type CliceClient } from "@clice/tools/client";
import { canonicalUri, Workspace } from "@clice/tools/workspace";
import { URI } from "vscode-uri";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const HEADER =
    "#pragma once\nint add(int a, int b);\nstruct Animal { virtual ~Animal() = default; };\n";
const MAIN = [
    '#include "a.h"',
    "int add(int a, int b) { return a + b; }",
    "struct Dog : Animal {};",
    "int compute() { return add(1, 2); }",
    "int main() { return compute(); }",
    "",
].join("\n");

interface Answer<T> {
    result?: T;
    error?: string;
    stale: string[];
}

function writeProject(session: { tmpdir(): Workspace }): Workspace {
    const ws = session.tmpdir();
    ws.write("a.h", HEADER);
    ws.write("main.cpp", MAIN);
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    return ws;
}

/// Answers spell paths the way the index does (canonical, forward slashes,
/// a lowercase Windows drive); the harness's URI form compares them.
function asUri(path: string): string {
    return canonicalUri(URI.file(path).toString());
}

function runClice(...args: string[]) {
    return spawnSync(cliceExecutable(), args, {
        encoding: "utf8",
        timeout: 120_000,
        maxBuffer: 64 * 1024 * 1024,
    });
}

function runIndex(ws: Workspace) {
    return runClice("index", "--workspace", ws.root, "--workers", "2");
}

/// How many translation units a batch index run indexed.
function indexedUnits(ws: Workspace): number {
    const run = runIndex(ws);
    expect(run.status, run.stderr).toBe(0);
    return Number(/Indexed (\d+) translation unit/.exec(run.stdout)?.[1]);
}

function query<T>(
    ws: Workspace,
    method: string,
    ...args: string[]
): Answer<T> & { status: number | null } {
    const run = runClice("query", "--workspace", ws.root, "--method", method, ...args);
    expect(run.stdout, `stderr: ${run.stderr}`).not.toBe("");
    return { ...(JSON.parse(run.stdout) as Answer<T>), status: run.status };
}

/// The references to the symbol at `place`, declarations included, as
/// sorted `file:line` sites.
function referenceSites(ws: Workspace, place: string): string[] {
    const refs = query<{ references: { file: string; line: number }[] }>(
        ws,
        "references",
        "--name",
        place,
        "--include-declaration",
    );
    expect(refs.status, refs.error).toBe(0);
    return refs.result!.references.map((r) => `${basename(r.file)}:${r.line}`).sort();
}

async function waitSymbol(client: CliceClient, name: string): Promise<boolean> {
    return waitUntil(
        async () => {
            const symbols = await client.workspaceSymbols(name);
            return symbols?.some((symbol) => symbol.name === name) ?? false;
        },
        { timeout: 30_000, interval: 500, description: `workspace symbol ${name}` },
    );
}

test("answers from the persisted index", ({ session }) => {
    const ws = writeProject(session);
    expect(runIndex(ws).status).toBe(0);

    const search = query<{
        symbols: { name: string; kind: string; line: number; symbolId: string }[];
    }>(ws, "symbolSearch", "--query", "add");
    expect(search.status).toBe(0);
    const add = search.result!.symbols.find((s) => s.name === "add")!;
    expect(add.kind).toBe("Function");
    expect(add.line).toBe(2);
    expect(add.symbolId).toMatch(/^#[0-9a-f]{16}$/);

    const byId = query<{ name: string }>(ws, "definition", "--symbol", add.symbolId);
    expect(byId.result?.name).toBe("add");

    const byLine = query<{ name: string }>(ws, "definition", "--path", "main.cpp", "--line", "4");
    expect(byLine.result?.name).toBe("compute");

    const read = query<{ text: string; startLine: number }>(ws, "readSymbol", "--name", "compute");
    expect(read.result?.text).toContain("add(1, 2)");
    expect(read.result?.startLine).toBe(4);

    const refs = query<{ total: number; references: { file: string; line: number }[] }>(
        ws,
        "references",
        "--name",
        "add",
        "--include-declaration",
    );
    expect(refs.result?.references.map((r) => r.line).sort()).toEqual([2, 2, 4]);

    const callers = query<{ callers: { name: string }[] }>(
        ws,
        "callGraph",
        "--name",
        "add",
        "--direction",
        "callers",
    );
    expect(callers.result?.callers.map((c) => c.name)).toEqual(["compute"]);

    const bases = query<{ supertypes: { name: string }[] }>(
        ws,
        "typeHierarchy",
        "--name",
        "Dog",
        "--direction",
        "supertypes",
    );
    expect(bases.result?.supertypes.map((t) => t.name)).toEqual(["Animal"]);

    const outline = query<{ symbols: { name: string }[] }>(
        ws,
        "documentSymbols",
        "--path",
        "main.cpp",
    );
    expect(outline.result?.symbols.map((s) => s.name).sort()).toEqual([
        "Dog",
        "add",
        "compute",
        "main",
    ]);

    const command = query<{
        file: string;
        arguments: string[];
        source: string;
        toolchainError: string | null;
    }>(ws, "compileCommand", "--path", "main.cpp");
    expect(asUri(command.result!.file)).toBe(ws.uri("main.cpp"));
    expect(command.result?.source).toBe("database");
    expect(command.result?.toolchainError).toBeNull();
    expect(command.result?.arguments).toContain("-cc1");

    const kinds = query<{ symbols: { name: string; kind: string }[] }>(
        ws,
        "symbolSearch",
        "--kind",
        "Struct,Function",
        "--limit",
        "10",
    );
    expect(kinds.result?.symbols.map((s) => s.kind).sort()).toEqual([
        "Function",
        "Function",
        "Function",
        "Struct",
        "Struct",
    ]);

    const files = query<{ files: { path: string; kind: string }[] }>(
        ws,
        "projectFiles",
        "--filter",
        "source",
    );
    expect(files.result?.files.map((f) => f.kind)).toEqual(["source"]);

    const deps = query<{ includes: { path: string; depth: number }[] }>(
        ws,
        "fileDeps",
        "--path",
        "main.cpp",
        "--direction",
        "includes",
    );
    expect(deps.result?.includes.map((d) => asUri(d.path))).toEqual([ws.uri("a.h")]);
});

test("workspace spelled with a climb", ({ session }) => {
    const ws = writeProject(session);
    ws.mkdir("build");
    expect(runIndex(ws).status).toBe(0);
    const ask = <T>(method: string, ...args: string[]): Answer<T> => {
        const run = spawnSync(
            cliceExecutable(),
            ["query", "--workspace", "..", "--method", method, ...args],
            { cwd: ws.path("build"), encoding: "utf8", timeout: 120_000 },
        );
        expect(run.stdout, `stderr: ${run.stderr}`).not.toBe("");
        return JSON.parse(run.stdout) as Answer<T>;
    };

    const animal = ask<{ definition: { file: string } }>("definition", "--name", "Animal");
    expect(asUri(animal.result!.definition.file)).toBe(ws.uri("a.h"));
    const files = ask<{ files: { path: string }[] }>("projectFiles");
    expect(files.result?.files.map((file) => asUri(file.path)).sort()).toEqual([
        ws.uri("a.h"),
        ws.uri("main.cpp"),
    ]);
});

test.skipIf(process.platform === "win32")("compile command through a symlink", ({ session }) => {
    const ws = session.tmpdir();
    ws.write("real/main.cpp", "int main() { return 0; }\n");
    ws.writeCDB(["real/main.cpp"]);
    ws.write(
        "clice.toml",
        '[project]\ncache_dir = "${workspace}/.clice"\n\n' +
            '[[rules]]\npatterns = ["real/**"]\nappend = ["-DFROM_RULE"]\n',
    );
    fs.symlinkSync(ws.path("real"), ws.path("link"));
    expect(runIndex(ws).status).toBe(0);

    const command = query<{ arguments: string[] }>(ws, "compileCommand", "--path", "link/main.cpp");
    expect(command.result?.arguments, "the rule matching the file's identity applies").toContain(
        "FROM_RULE",
    );
});

test.skipIf(process.platform === "win32")(
    "a header lends from the include that finds it",
    ({ session }) => {
        const ws = session.tmpdir();
        ws.write("a/main.cpp", "int main() { return 0; }\n");
        ws.write("vendor/b.cpp", "int b() { return 0; }\n");
        ws.write("vendor/real/orphan.h", "int orphan();\n");
        fs.symlinkSync(ws.path("vendor/real"), ws.path("a/inc"));
        ws.writeEntries([
            ["a/main.cpp", ["-DFROM_A", `-I${ws.path("a/inc")}`]],
            ["vendor/b.cpp", ["-DFROM_B"]],
        ]);
        ws.pinCacheDir();
        expect(runIndex(ws).status).toBe(0);

        const command = query<{ arguments: string[] }>(
            ws,
            "compileCommand",
            "--path",
            "a/inc/orphan.h",
        );
        expect(command.result?.arguments, "a's search reaches the header").toContain("FROM_A");
    },
);

test("names a failed compiler query", ({ session }) => {
    const ws = session.tmpdir();
    ws.write("main.cpp", "int main() { return 0; }\n");
    const driver = ws.path("missing-cc");
    ws.write(
        "compile_commands.json",
        JSON.stringify([
            {
                directory: ws.root,
                file: ws.path("main.cpp"),
                arguments: [driver, "-c", "main.cpp"],
            },
        ]),
    );
    ws.pinCacheDir();
    expect(runIndex(ws).status, "the unit still parses from its driver-level command").toBe(0);

    const command = query<{ arguments: string[]; toolchainError: string | null }>(
        ws,
        "compileCommand",
        "--path",
        "main.cpp",
    );
    expect(command.result?.toolchainError).toContain("missing-cc");
    expect(command.result?.arguments).not.toContain("-cc1");
});

test("answers for a file only its own symbols name", ({ session }) => {
    // Nothing in the global table references the file, so only the
    // fetch of its own shard can answer for it.
    const ws = session.tmpdir();
    ws.write("local.cpp", "static int helper() { return 7; }\nint main() { return helper(); }\n");
    ws.writeCDB(["local.cpp"]);
    ws.pinCacheDir();
    expect(runIndex(ws).status).toBe(0);

    const onLine = query<{ symbols: { name: string }[] }>(
        ws,
        "symbolSearch",
        "--query",
        "local.cpp:1",
    );
    expect(onLine.status).toBe(0);
    expect(onLine.result?.symbols.map((s) => s.name)).toEqual(["helper"]);
    expect(onLine.stale).toEqual([]);
});

test("references reach internal and local symbols", ({ session }) => {
    const ws = session.tmpdir();
    ws.write("util.h", "#pragma once\nstatic int helper(int x) { return x; }\n");
    ws.write("a.cpp", '#include "util.h"\nint a() { return helper(1); }\n');
    ws.write(
        "b.cpp",
        '#include "util.h"\nint b() { int local = 2; return helper(local) + local; }\n',
    );
    ws.writeCDB(["a.cpp", "b.cpp"]);
    ws.pinCacheDir();
    expect(runIndex(ws).status).toBe(0);

    const helper = ["a.cpp:2", "b.cpp:2", "util.h:2"];
    expect(referenceSites(ws, "util.h:2:12")).toEqual(helper);
    expect(referenceSites(ws, "a.cpp:2:18")).toEqual(helper);
    expect(referenceSites(ws, "b.cpp:2:15")).toEqual(["b.cpp:2", "b.cpp:2", "b.cpp:2"]);

    const outline = (file: string) =>
        query<{ symbols: { name: string; symbolId: string }[] }>(
            ws,
            "documentSymbols",
            "--path",
            file,
        ).result!.symbols;
    const [listed] = outline("util.h");
    expect(listed?.name).toBe("helper");
    expect(outline("b.cpp").map((s) => s.name)).toEqual(["b"]);

    // An internal symbol's id names it together with the file it was found in.
    const byId = query<{ name: string }>(
        ws,
        "definition",
        "--symbol",
        listed!.symbolId,
        "--path",
        "util.h",
    );
    expect(byId.status, byId.error).toBe(0);
    expect(byId.result?.name).toBe("helper");
});

test("references reach enumerators of every file", ({ session }) => {
    const unnamed = session.tmpdir();
    unnamed.write("flags.h", "#pragma once\nenum { flag = 1 };\n");
    unnamed.write("a.cpp", '#include "flags.h"\nint a() { return flag; }\n');
    unnamed.write("b.cpp", '#include "flags.h"\nint b() { return flag * 2; }\n');
    unnamed.writeCDB(["a.cpp", "b.cpp"]);
    unnamed.pinCacheDir();
    expect(runIndex(unnamed).status).toBe(0);
    expect(referenceSites(unnamed, "a.cpp:2:18")).toEqual(["a.cpp:2", "b.cpp:2", "flags.h:2"]);

    const c = session.tmpdir();
    c.write("color.h", "#pragma once\nenum color { red };\n");
    c.write("a.c", '#include "color.h"\nint a(void) { return red; }\n');
    c.write("b.c", '#include "color.h"\nint b(void) { return red + 1; }\n');
    c.writeCDB(["a.c", "b.c"], { std: "c17", extraArgs: ["-x", "c"] });
    c.pinCacheDir();
    expect(runIndex(c).status).toBe(0);
    expect(referenceSites(c, "a.c:2:22")).toEqual(["a.c:2", "b.c:2", "color.h:2"]);
});

test("context of a name opening its line", ({ session }) => {
    const ws = session.tmpdir();
    ws.write("main.cpp", "int\nvalue() { return 1; }\nint use() { return value(); }\n");
    ws.writeCDB(["main.cpp"]);
    ws.pinCacheDir();
    expect(runIndex(ws).status).toBe(0);

    const refs = query<{ references: { context: string }[] }>(
        ws,
        "references",
        "--name",
        "value",
        "--include-declaration",
    );
    expect(refs.result?.references.map((r) => r.context).sort()).toEqual([
        "int use() { return value(); }",
        "value() { return 1; }",
    ]);
});

test("moved checkout keeps its index", ({ session }) => {
    const parent = session.tmpdir();
    const before = new Workspace(parent.path("before"));
    const after = new Workspace(parent.path("after"));
    // Written the way a build generator writes it: every path absolute.
    const writeCDB = (ws: Workspace) => {
        ws.writeCDB(["src/a.cpp", "src/b.cpp"], { extraArgs: [`-I${ws.path("inc")}`] });
    };
    before.write("inc/util.h", "#pragma once\n#define UTIL_LIMIT 4\n");
    before.write(
        "src/a.cpp",
        '#include "util.h"\nstatic int helper() { return UTIL_LIMIT; }\nint use_a() { return helper(); }\n',
    );
    before.write("src/b.cpp", '#include "util.h"\nint use_b() { return UTIL_LIMIT; }\n');
    before.pinCacheDir();
    writeCDB(before);
    const ids = (ws: Workspace) =>
        ["UTIL_LIMIT", "src/a.cpp:2"].map(
            (text) =>
                query<{ symbols: { symbolId: string }[] }>(ws, "symbolSearch", "--query", text)
                    .result?.symbols[0]?.symbolId,
        );
    expect(indexedUnits(before)).toBe(2);
    const first = ids(before);
    expect(first.every((id) => id !== undefined)).toBe(true);

    fs.renameSync(before.root, after.root);
    writeCDB(after);
    expect(indexedUnits(after), "the moved index is current").toBe(0);
    expect(ids(after), "a macro and a file-local symbol keep their ids").toEqual(first);
});

// Only Linux file systems take a name that is not UTF-8.
test.skipIf(process.platform !== "linux")(
    "a path that is not UTF-8 still sees command edits",
    ({ session }) => {
        const ws = session.tmpdir();
        const target = Buffer.concat([
            Buffer.from(`${ws.root}/`),
            Buffer.from([0xff]),
            Buffer.from(".cpp"),
        ]);
        fs.writeFileSync(target, "int f() { return X; }\n");
        fs.symlinkSync(target, ws.path("a.cpp"));
        ws.pinCacheDir();
        ws.writeCDB(["a.cpp"], { extraArgs: ["-DX=1"] });
        expect(indexedUnits(ws)).toBe(1);

        ws.writeCDB(["a.cpp"], { extraArgs: ["-DX=2"] });
        expect(indexedUnits(ws), "the edited command reindexes the file").toBe(1);
    },
);

test("rejects bad questions", ({ session }) => {
    const ws = writeProject(session);
    expect(runIndex(ws).status).toBe(0);

    const direction = query(ws, "callGraph", "--name", "add", "--direction", "sideways");
    expect(direction.status).toBe(1);
    expect(direction.error).toContain("sideways");

    const unknown = query(ws, "definition", "--name", "nope");
    expect(unknown.status).toBe(1);
    expect(unknown.error).toBe("symbol not found");

    const missing = query(ws, "documentSymbols", "--path", "gone.cpp");
    expect(missing.status).toBe(1);
    expect(missing.error).toContain("no such file");

    const deps = query(ws, "fileDeps", "--path", "gone.cpp");
    expect(deps.status).toBe(1);
    expect(deps.error).toContain("no such file");

    const line = query(ws, "definition", "--path", "main.cpp", "--line", "0");
    expect(line.status).toBe(1);
    expect(line.error).toContain("positive");

    const typo = query(ws, "definition", "--name", "add", "--path", "gone.cpp");
    expect(typo.status).toBe(1);
    expect(typo.error).toContain("no such file");

    const kind = query(ws, "symbolSearch", "--query", "add", "--kind", "Fnction");
    expect(kind.status).toBe(1);
    expect(kind.error).toContain("Fnction");

    const method = query(ws, "bogus");
    expect(method.status).toBe(1);
    expect(method.error).toContain("bogus");

    const noIndex = query(writeProject(session), "symbolSearch", "--query", "add");
    expect(noIndex.status).toBe(1);
    expect(noIndex.error).toContain("could not be opened");
});

test("withholds rows the disk moved on from", ({ session }) => {
    const ws = writeProject(session);
    expect(runIndex(ws).status).toBe(0);
    ws.write("main.cpp", MAIN.replace("int main()", "int extra() { return 7; }\nint main()"));

    // The definition sits in the edited file: its rows would point at
    // text that moved, so the symbol is unfindable and the file named.
    const stale = query(ws, "definition", "--name", "compute");
    expect(stale.status).toBe(1);
    expect(stale.stale.map(asUri)).toEqual([ws.uri("main.cpp")]);

    const outline = query<{ symbols: unknown[] }>(ws, "documentSymbols", "--path", "main.cpp");
    expect(outline.result?.symbols).toEqual([]);
    expect(outline.stale.map(asUri)).toEqual([ws.uri("main.cpp")]);

    const header = query<{ symbols: { name: string }[] }>(ws, "documentSymbols", "--path", "a.h");
    expect(header.result?.symbols.map((s) => s.name)).toContain("Animal");
    expect(header.stale).toEqual([]);
});

test("fresh runs the batch indexer", ({ session }) => {
    const ws = writeProject(session);

    // No index yet: --fresh builds it.
    const first = query<{ symbols: { name: string }[] }>(
        ws,
        "symbolSearch",
        "--query",
        "compute",
        "--fresh",
    );
    expect(first.status).toBe(0);
    expect(first.result?.symbols.map((s) => s.name)).toEqual(["compute"]);

    ws.write("main.cpp", MAIN.replace("int main()", "int extra() { return 7; }\nint main()"));
    const second = query<{ symbols: { name: string }[] }>(
        ws,
        "symbolSearch",
        "--query",
        "extra",
        "--fresh",
    );
    expect(second.status).toBe(0);
    expect(second.result?.symbols.map((s) => s.name)).toEqual(["extra"]);
    expect(second.stale).toEqual([]);
});

test("asks the running server to index", async ({ session }) => {
    const ws = writeProject(session);
    const client = await session.spawn(ws).initialize(ws);
    expect(await waitSymbol(client, "compute"), "server never indexed").toBe(true);
    expect(fs.existsSync(ws.path(".clice/server.json"))).toBe(true);

    // The server holds the writer lock, so the batch command delegates.
    const delegated = runIndex(ws);
    expect(delegated.status, `stderr: ${delegated.stderr}`).toBe(0);
    expect(delegated.stdout).toContain("through the running clice server");

    ws.write("main.cpp", MAIN.replace("int main()", "int extra() { return 7; }\nint main()"));
    const fresh = query<{ symbols: { name: string }[] }>(
        ws,
        "symbolSearch",
        "--query",
        "extra",
        "--fresh",
    );
    expect(fresh.status).toBe(0);
    expect(fresh.result?.symbols.map((s) => s.name)).toEqual(["extra"]);

    const persisted = query<{ symbols: { name: string }[] }>(
        ws,
        "symbolSearch",
        "--query",
        "extra",
    );
    expect(persisted.result?.symbols.map((s) => s.name)).toEqual(["extra"]);

    await client.shutdown();
    expect(fs.existsSync(ws.path(".clice/server.json"))).toBe(false);
});

test("asked index finds later database", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("a.h", HEADER);
    ws.write("main.cpp", MAIN);
    ws.pinCacheDir();
    const client = await session.spawn(ws).initialize(ws);
    await waitUntil(() => fs.existsSync(ws.path(".clice/server.json")), {
        timeout: 30_000,
        interval: 100,
        description: "the server's control endpoint",
    });

    const empty = runIndex(ws);
    expect(empty.stderr).toContain("has no translation units");

    ws.writeCDB(["main.cpp"]);
    const delegated = runIndex(ws);
    expect(delegated.status, `stderr: ${delegated.stderr}`).toBe(0);
    expect(await waitSymbol(client, "compute"), "server never indexed").toBe(true);
});

test("refuses a writer it cannot ask", async ({ session }) => {
    const ws = writeProject(session);
    const client = await session.spawn(ws).initialize(ws);
    expect(await waitSymbol(client, "compute"), "server never indexed").toBe(true);

    // A lock holder without a record (a batch run, a server of another
    // build) cannot be asked: the commands that need the writer give up.
    fs.rmSync(ws.path(".clice/server.json"));
    const refused = runIndex(ws);
    expect(refused.status).toBe(1);
    expect(refused.stderr).toContain("holds the index writer lock");

    const fresh = query(ws, "symbolSearch", "--query", "compute", "--fresh");
    expect(fresh.status).toBe(1);
    expect(fresh.error).toContain("holds the index writer lock");

    // Reads never wait for the writer; they see the disk, which trails the
    // server's memory by at most one indexing round.
    const persisted = await waitUntil(
        () => {
            const plain = query<{ symbols: { name: string }[] }>(
                ws,
                "symbolSearch",
                "--query",
                "compute",
            );
            return plain.result?.symbols.some((s) => s.name === "compute") ?? false;
        },
        { timeout: 30_000, interval: 500, description: "persisted rows for compute" },
    );
    expect(persisted).toBe(true);
});

test("fresh names the units it could not index", ({ session }) => {
    const ws = writeProject(session);
    // A database entry whose file does not exist never indexes.
    ws.writeCDB(["main.cpp", "ghost.cpp"]);

    // A unit that fails to index has no rows: the answer lists it next to
    // the withheld files rather than passing silence off as completeness.
    const fresh = query<{ symbols: { name: string }[] }>(
        ws,
        "symbolSearch",
        "--query",
        "ghost",
        "--fresh",
    );
    expect(fresh.status).toBe(0);
    expect(fresh.result?.symbols).toEqual([]);
    expect(fresh.stale.map(asUri)).toEqual([ws.uri("ghost.cpp")]);
});

test("delegation keeps the configuration", async ({ session }) => {
    const ws = writeProject(session);
    ws.write(
        "clice.toml",
        [
            "[project]",
            'cache_dir = "${workspace}/.clice"',
            "",
            "[[rules]]",
            'configuration = "debug"',
            'patterns = ["**/*.cpp"]',
            'append = ["-DDEBUG"]',
            "",
            "[[rules]]",
            'configuration = "release"',
            'patterns = ["**/*.cpp"]',
            'append = ["-DRELEASE"]',
            "",
        ].join("\n"),
    );
    const client = await session
        .spawn(ws, { args: ["serve", "--configuration", "debug"] })
        .initialize(ws);
    expect(await waitSymbol(client, "compute"), "server never indexed").toBe(true);

    // The server indexes one configuration; asking it for another is
    // refused rather than answered with the wrong build.
    const other = runClice("index", "--workspace", ws.root, "--configuration", "release");
    expect(other.status).toBe(1);
    expect(other.stderr).toContain("configuration 'debug'");

    const same = runClice("index", "--workspace", ws.root, "--configuration", "debug");
    expect(same.status, `stderr: ${same.stderr}`).toBe(0);
    expect(same.stdout).toContain("through the running clice server");
});
