/// Tests for the agentic protocol handlers.

import { findFreePort, SETTLE_TIME, sleep, waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test, type SessionFactory } from "../fixtures.ts";
import { AgenticRpcClient, jsonSafe, posix, runAgentic } from "./rpc.ts";

interface SymbolInfo {
    name: string;
    kind: string;
    line: number;
    symbolId: number;
    file: string;
}

interface GraphNode {
    name: string;
    line: number;
    symbolId: number;
}

/// Spawn a server with the agentic TCP side channel bound to a free port.
async function agenticServer(
    session: SessionFactory,
    name: string,
    initializationOptions?: Record<string, unknown>,
): Promise<{ host: string; port: number; client: CliceClient; workspace: Workspace }> {
    const host = "127.0.0.1";
    const port = await findFreePort();
    const { client, workspace } = await session(name, {
        args: ["serve", "--host", host, "--port", String(port)],
        ...(initializationOptions === undefined ? {} : { initializationOptions }),
    });
    return { host, port, client, workspace };
}

/// Start server with LSP+agentic, compile a file, wait for indexing, then
/// hand back a connected RPC client.
async function indexedAgentic(
    session: SessionFactory,
): Promise<{ rpc: AgenticRpcClient; client: CliceClient; workspace: Workspace; uri: string }> {
    // The first agentic query triggers a catch-up indexing round for open
    // files; the default 3s idle timer would dominate every fixture setup.
    const { host, port, client, workspace } = await agenticServer(session, "index_features", {
        project: { idle_timeout_ms: 10 },
    });

    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "add"), "Index not ready").toBe(true);

    const rpc = new AgenticRpcClient();
    await rpc.connect(host, port);

    await waitUntil(
        async () => {
            const resp = await rpc.request<{ symbols: unknown[] }>("agentic/symbolSearch", {
                query: "add",
            });
            return resp.result?.symbols.length ?? 0;
        },
        {
            timeout: 30_000,
            interval: 100,
            description: "agentic/symbolSearch to return indexed symbols",
        },
    );

    return { rpc, client, workspace, uri };
}

test("compile command", async ({ session }) => {
    const { host, port, workspace } = await agenticServer(session, "hello_world");
    const mainCpp = posix(workspace.path("main.cpp"));
    const result = await runAgentic(cliceExecutable(), host, port, mainCpp);
    expect(result.status, `stderr: ${result.stderr}`).toBe(0);
    const data = JSON.parse(result.stdout) as {
        file: string;
        directory: string;
        arguments: string[];
    };
    expect(data.file).toBe(mainCpp);
    expect(data.directory).toBe(posix(workspace.root));
    expect(data.arguments.length).toBeGreaterThan(0);
});

test("compile command fallback", async ({ session }) => {
    const { host, port, workspace } = await agenticServer(session, "hello_world");
    // Absolute on every platform; a relative path would be resolved
    // against the CLI's cwd before it reaches the server.
    const probe = posix(workspace.path("nonexistent/file.cpp"));
    const result = await runAgentic(cliceExecutable(), host, port, probe);
    expect(result.status, `stderr: ${result.stderr}`).toBe(0);
    const data = JSON.parse(result.stdout) as { file: string };
    expect(data.file).toBe(probe);
});

test("multiple requests", async ({ session }) => {
    const { host, port, workspace } = await agenticServer(session, "hello_world");
    const mainCpp = posix(workspace.path("main.cpp"));
    for (let i = 0; i < 3; i++) {
        const result = await runAgentic(cliceExecutable(), host, port, mainCpp);
        expect(result.status, `stderr: ${result.stderr}`).toBe(0);
        const data = JSON.parse(result.stdout) as { file: string };
        expect(data.file).toBe(mainCpp);
    }
});

test("connection refused", async () => {
    const port = await findFreePort();
    const result = await runAgentic(cliceExecutable(), "127.0.0.1", port, "/some/file.cpp");
    expect(result.status).not.toBe(0);
});

test("concurrent connections", async ({ session }) => {
    const { host, port, workspace } = await agenticServer(session, "hello_world");
    const mainCpp = posix(workspace.path("main.cpp"));
    const results = await Promise.all(
        [0, 1, 2, 3].map(() => runAgentic(cliceExecutable(), host, port, mainCpp)),
    );
    for (const r of results) {
        expect(r.status, `stderr: ${r.stderr}`).toBe(0);
        const data = JSON.parse(r.stdout) as { file: string };
        expect(data.file).toBe(mainCpp);
    }
});

test("rpc compile command", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const resp = await rpc.request<{ file: string; arguments: string[] }>(
            "agentic/compileCommand",
            { path: p },
        );
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        expect(resp.result!.file).toBe(p);
        expect(resp.result!.arguments.length).toBeGreaterThan(0);
    } finally {
        rpc.close();
    }
});

test("rpc project files", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ total: number; files: { path: string }[] }>(
            "agentic/projectFiles",
            {},
        );
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        expect(resp.result!.total).toBeGreaterThan(0);
        const paths = resp.result!.files.map((f) => f.path);
        expect(paths.some((p) => p.includes("main.cpp"))).toBe(true);
    } finally {
        rpc.close();
    }
});

test("rpc project files filter", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ files: { kind: string }[] }>("agentic/projectFiles", {
            filter: "source",
        });
        expect(resp.result).toBeDefined();
        for (const f of resp.result!.files) {
            expect(f.kind).toBe("source");
        }
    } finally {
        rpc.close();
    }
});

test("rpc symbol search", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ symbols: SymbolInfo[] }>("agentic/symbolSearch", {
            query: "add",
        });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const symbols = resp.result!.symbols;
        const addSym = symbols.find((s) => s.name === "add");
        expect(
            addSym,
            `'add' not found in ${JSON.stringify(symbols.map((s) => s.name))}`,
        ).toBeDefined();
        expect(addSym!.kind).toBe("Function");
        expect(addSym!.line).toBe(19);
        expect(addSym!.symbolId).not.toBe(0);
        expect(addSym!.file).toContain("main.cpp");
    } finally {
        rpc.close();
    }
});

test("rpc symbol search kind", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ symbols: SymbolInfo[] }>("agentic/symbolSearch", {
            query: "Animal",
            kindFilter: ["Struct"],
        });
        expect(resp.result).toBeDefined();
        for (const s of resp.result!.symbols) {
            expect(s.kind).toBe("Struct");
        }
    } finally {
        rpc.close();
    }
});

test("rpc symbol search max", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ symbols: SymbolInfo[] }>("agentic/symbolSearch", {
            query: "",
            maxResults: 3,
        });
        expect(resp.result).toBeDefined();
        expect(resp.result!.symbols.length).toBeLessThanOrEqual(3);
    } finally {
        rpc.close();
    }
});

test("rpc read symbol", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{
            name: string;
            symbolId: number;
            startLine: number;
            endLine: number;
            text: string;
        }>("agentic/readSymbol", { name: "add" });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.name).toBe("add");
        expect(result.symbolId).not.toBe(0);
        expect(result.startLine).toBe(19);
        expect(result.endLine).toBe(21);
        expect(result.text).toContain("int add(int a, int b)");
        expect(result.text).toContain("return a + b;");
    } finally {
        rpc.close();
    }
});

test("rpc read symbol by id", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp1 = await rpc.request<{ symbolId: number }>("agentic/readSymbol", {
            name: "add",
        });
        expect(resp1.result).toBeDefined();
        const sid = resp1.result!.symbolId;

        const resp2 = await rpc.request<{ name: string; symbolId: number }>("agentic/readSymbol", {
            symbolId: sid,
        });
        expect(resp2.result).toBeDefined();
        expect(resp2.result!.name).toBe("add");
        expect(resp2.result!.symbolId).toBe(sid);
    } finally {
        rpc.close();
    }
});

test("rpc document symbols", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const resp = await rpc.request<{ symbols: { name: string; kind: string }[] }>(
            "agentic/documentSymbols",
            { path: p },
        );
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const symbols = resp.result!.symbols;
        const names = symbols.map((s) => s.name);
        const kinds = symbols.map((s) => s.kind);
        expect(names, `expected 'add' in ${JSON.stringify(names)}`).toContain("add");
        expect(names, `expected 'main' in ${JSON.stringify(names)}`).toContain("main");
        expect(names, `expected 'global_var' in ${JSON.stringify(names)}`).toContain("global_var");
        expect(
            kinds,
            `Parameters should be filtered: ${JSON.stringify(symbols.map((s) => [s.name, s.kind]))}`,
        ).not.toContain("Parameter");
    } finally {
        rpc.close();
    }
});

test("rpc definition", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{
            name: string;
            definition: { file: string; startLine: number; endLine: number; text: string } | null;
        }>("agentic/definition", { name: "add" });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.name).toBe("add");
        expect(result.definition).not.toBeNull();
        const defn = result.definition!;
        expect(defn.file).toContain("main.cpp");
        expect(defn.startLine).toBe(19);
        expect(defn.endLine).toBe(21);
        expect(defn.text).toContain("int add(int a, int b)");
        expect(defn.text).toContain("return a + b;");
    } finally {
        rpc.close();
    }
});

test("rpc definition by position", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const resp = await rpc.request<{ name: string }>("agentic/definition", {
            path: p,
            line: 19,
        });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        expect(resp.result!.name).toBe("add");
    } finally {
        rpc.close();
    }
});

test("rpc references", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{
            name: string;
            total: number;
            references: { line: number; context: string }[];
        }>("agentic/references", { name: "global_var" });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.name).toBe("global_var");
        expect(result.total).toBe(2);
        const lines = result.references.map((r) => r.line).sort((a, b) => a - b);
        expect(lines).toEqual([34, 38]);
        const contexts = result.references.map((r) => r.context);
        expect(contexts.some((c) => c.includes("global_var + 1"))).toBe(true);
        expect(contexts.some((c) => c.includes("global_var * 2"))).toBe(true);
    } finally {
        rpc.close();
    }
});

test("rpc references include decl", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ total: number; references: { line: number }[] }>(
            "agentic/references",
            { name: "global_var", includeDeclaration: true },
        );
        expect(resp.result).toBeDefined();
        const result = resp.result!;
        expect(result.total).toBe(3);
        const lines = result.references.map((r) => r.line).sort((a, b) => a - b);
        expect(lines, `expected declaration line 31 in ${JSON.stringify(lines)}`).toContain(31);
    } finally {
        rpc.close();
    }
});

test("rpc call graph incoming", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{
            root: GraphNode;
            callers: GraphNode[];
            callees: GraphNode[];
        }>("agentic/callGraph", { name: "add", direction: "callers" });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.root.name).toBe("add");
        expect(result.root.line).toBe(19);
        expect(result.root.symbolId).not.toBe(0);
        const callerNames = result.callers.map((c) => c.name);
        expect(callerNames, `expected 'compute' in ${JSON.stringify(callerNames)}`).toContain(
            "compute",
        );
        const compute = result.callers.find((c) => c.name === "compute")!;
        expect(compute.line).toBe(24);
        expect(compute.symbolId).not.toBe(0);
        expect(result.callees).toEqual([]);
    } finally {
        rpc.close();
    }
});

test("rpc call graph outgoing", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{
            root: GraphNode;
            callers: GraphNode[];
            callees: GraphNode[];
        }>("agentic/callGraph", { name: "compute", direction: "callees" });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.root.name).toBe("compute");
        const calleeNames = result.callees.map((c) => c.name);
        expect(calleeNames, `expected 'add' in ${JSON.stringify(calleeNames)}`).toContain("add");
        const addEntry = result.callees.find((c) => c.name === "add")!;
        expect(addEntry.line).toBe(19);
        expect(result.callers).toEqual([]);
    } finally {
        rpc.close();
    }
});

test("rpc type hierarchy supertypes", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ root: GraphNode; supertypes: GraphNode[] }>(
            "agentic/typeHierarchy",
            { name: "Dog", direction: "supertypes" },
        );
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.root.name).toBe("Dog");
        expect(result.root.line).toBe(9);
        const supertypeNames = result.supertypes.map((t) => t.name);
        expect(supertypeNames, `expected 'Animal' in ${JSON.stringify(supertypeNames)}`).toContain(
            "Animal",
        );
        const animal = result.supertypes.find((t) => t.name === "Animal")!;
        expect(animal.line).toBe(2);
        expect(animal.symbolId).not.toBe(0);
    } finally {
        rpc.close();
    }
});

test("rpc type hierarchy subtypes", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ root: GraphNode; subtypes: GraphNode[] }>(
            "agentic/typeHierarchy",
            { name: "Animal", direction: "subtypes" },
        );
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.root.name).toBe("Animal");
        expect(result.root.line).toBe(2);
        const subtypeNames = result.subtypes.map((t) => t.name);
        expect(subtypeNames, `expected 'Dog' in ${JSON.stringify(subtypeNames)}`).toContain("Dog");
        expect(subtypeNames, `expected 'Cat' in ${JSON.stringify(subtypeNames)}`).toContain("Cat");
        const dog = result.subtypes.find((t) => t.name === "Dog")!;
        expect(dog.line).toBe(9);
        const cat = result.subtypes.find((t) => t.name === "Cat")!;
        expect(cat.line).toBe(14);
    } finally {
        rpc.close();
    }
});

test("rpc status", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{
            idle: boolean;
            total: number;
            pending: number;
            indexed: number;
        }>("agentic/status", {});
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(typeof result.idle).toBe("boolean");
        expect(result.total).toBeGreaterThan(0);
        expect(Number.isInteger(result.pending)).toBe(true);
        expect(Number.isInteger(result.indexed)).toBe(true);
    } finally {
        rpc.close();
    }
});

/// Shutdown notification should cause the server to exit cleanly.
test("rpc shutdown", async ({ session }) => {
    const host = "127.0.0.1";
    const port = await findFreePort();
    const { client } = await session("hello_world", {
        args: ["serve", "--host", host, "--port", String(port)],
    });

    const rpc = new AgenticRpcClient();
    await rpc.connect(host, port);
    rpc.notify("agentic/shutdown", {});
    rpc.close();

    await client.assertExitedCleanly();
    // The server is already gone; drop the stdio transport so the session
    // teardown's shutdown request cannot write to a destroyed stream.
    client.dispose();
});

test("rpc symbol not found", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request("agentic/definition", { name: "nonexistent_symbol_xyz" });
        expect(resp.error).toBeDefined();
    } finally {
        rpc.close();
    }
});

/// Search -> get symbolId -> definition -> verify consistency.
test("rpc symbol id roundtrip", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const search = await rpc.request<{ symbols: SymbolInfo[] }>("agentic/symbolSearch", {
            query: "compute",
        });
        expect(search.result).toBeDefined();
        const symbols = search.result!.symbols;
        const compute = symbols.find((s) => s.name === "compute");
        expect(
            compute,
            `'compute' not found in ${JSON.stringify(symbols.map((s) => s.name))}`,
        ).toBeDefined();

        const defn = await rpc.request<{ name: string; symbolId: number }>("agentic/definition", {
            symbolId: compute!.symbolId,
        });
        expect(defn.result).toBeDefined();
        expect(defn.result!.name).toBe("compute");
        expect(defn.result!.symbolId).toBe(compute!.symbolId);
    } finally {
        rpc.close();
    }
});

test("rpc file deps", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const resp = await rpc.request<{ file: string; includes: unknown[]; includers: unknown[] }>(
            "agentic/fileDeps",
            { path: p },
        );
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(result.file).toBe(p);
        expect(Array.isArray(result.includes)).toBe(true);
        expect(Array.isArray(result.includers)).toBe(true);
    } finally {
        rpc.close();
    }
});

// Agentic clients pass native path spellings; on Windows an uppercase
// drive must hit the same pool entry as the lowercase canonical form
// (the lookup goes through PathPool::find, not a raw map probe).
test.skipIf(process.platform !== "win32")("rpc file deps native spelling", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const native = p.charAt(0).toUpperCase() + p.slice(1);
        const resp = await rpc.request<{ file: string; includes?: unknown[] }>("agentic/fileDeps", {
            path: native,
        });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        // A pool miss echoes back only the file field; a hit carries the
        // dependency arrays (possibly empty). The native spelling must hit.
        expect(Array.isArray(resp.result!.includes)).toBe(true);
    } finally {
        rpc.close();
    }
});

test("rpc file deps direction", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const resp = await rpc.request<{ includers: unknown[] }>("agentic/fileDeps", {
            path: p,
            direction: "includes",
        });
        expect(resp.result).toBeDefined();
        expect(resp.result!.includers).toEqual([]);
    } finally {
        rpc.close();
    }
});

test("rpc file deps unknown", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ includes: unknown[]; includers: unknown[] }>(
            "agentic/fileDeps",
            { path: "/nonexistent/file.cpp" },
        );
        expect(resp.result).toBeDefined();
        expect(resp.result!.includes).toEqual([]);
        expect(resp.result!.includers).toEqual([]);
    } finally {
        rpc.close();
    }
});

test("rpc impact analysis", async ({ session }) => {
    const { rpc, workspace } = await indexedAgentic(session);
    try {
        const p = posix(workspace.path("main.cpp"));
        const resp = await rpc.request<{
            directDependents: unknown[];
            transitiveDependents: unknown[];
            affectedModules: unknown[];
        }>("agentic/impactAnalysis", { path: p });
        expect(resp.result, `unexpected response: ${jsonSafe(resp)}`).toBeDefined();
        const result = resp.result!;
        expect(Array.isArray(result.directDependents)).toBe(true);
        expect(Array.isArray(result.transitiveDependents)).toBe(true);
        expect(Array.isArray(result.affectedModules)).toBe(true);
    } finally {
        rpc.close();
    }
});

test("rpc impact analysis unknown", async ({ session }) => {
    const { rpc } = await indexedAgentic(session);
    try {
        const resp = await rpc.request<{ directDependents: unknown[] }>("agentic/impactAnalysis", {
            path: "/nonexistent/file.cpp",
        });
        expect(resp.result).toBeDefined();
        expect(resp.result!.directDependents).toEqual([]);
    } finally {
        rpc.close();
    }
});

/// Shutdown during active background indexing must exit cleanly.
test("shutdown during indexing", async ({ session }) => {
    const workspace = session.tmpdir();
    const entries: { directory: string; file: string; arguments: string[] }[] = [];
    for (let i = 0; i < 20; i++) {
        const name = `file_${i}.cpp`;
        workspace.write(
            name,
            `struct Type_${i} { int v = ${i}; void m() {} };\n` +
                `int func_${i}(int x) { return x + ${i}; }\n` +
                `int caller_${i}() { return func_${i}(${i}); }\n`,
        );
        const src = posix(workspace.path(name));
        entries.push({
            directory: posix(workspace.root),
            file: src,
            arguments: ["clang++", "-std=c++17", "-fsyntax-only", src],
        });
    }
    workspace.write("compile_commands.json", JSON.stringify(entries));

    const host = "127.0.0.1";
    const port = await findFreePort();
    const client = session.spawn(workspace, {
        args: ["serve", "--host", host, "--port", String(port)],
    });

    try {
        try {
            await client.initialize(workspace, {
                initializationOptions: { project: { idle_timeout_ms: 0 } },
            });
        } catch (exc) {
            if (client.child.exitCode !== null) {
                await client.assertExitedCleanly(15_000);
            }
            throw exc;
        }

        // Give indexing a moment to start, then send shutdown.
        await sleep(SETTLE_TIME);

        const rpc = new AgenticRpcClient();
        await rpc.connect(host, port);
        rpc.notify("agentic/shutdown", {});
        rpc.close();

        await client.assertExitedCleanly(15_000);
    } finally {
        // The server is already gone; drop the stdio transport so the session
        // teardown's shutdown request cannot write to a destroyed stream.
        client.dispose();
    }
});
