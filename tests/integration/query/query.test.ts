/// `clice query` reads the persisted index straight from disk: answers
/// carry the files whose rows were withheld because the disk moved on,
/// and --fresh brings the index up to date first — through the running
/// server when one holds the writer lock, else by a batch run.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import { waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
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

function query<T>(
    ws: Workspace,
    method: string,
    ...args: string[]
): Answer<T> & { status: number | null } {
    const run = runClice("query", "--workspace", ws.root, "--method", method, ...args);
    expect(run.stdout, `stderr: ${run.stderr}`).not.toBe("");
    return { ...(JSON.parse(run.stdout) as Answer<T>), status: run.status };
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

    const command = query<{ file: string; arguments: string[]; source: string }>(
        ws,
        "compileCommand",
        "--path",
        "main.cpp",
    );
    expect(command.result?.file).toBe(ws.path("main.cpp"));
    expect(command.result?.source).toBe("database");
    expect(command.result?.arguments.length).toBeGreaterThan(0);

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
    expect(deps.result?.includes.map((d) => d.path)).toEqual([ws.path("a.h")]);
});

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
    expect(stale.stale).toEqual([ws.path("main.cpp")]);

    const outline = query<{ symbols: unknown[] }>(ws, "documentSymbols", "--path", "main.cpp");
    expect(outline.result?.symbols).toEqual([]);
    expect(outline.stale).toEqual([ws.path("main.cpp")]);

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
    expect(fresh.stale).toEqual([ws.path("ghost.cpp")]);
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
