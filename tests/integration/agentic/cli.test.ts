/// CLI-based tests for agentic mode and CLI entry points.

import { findFreePort, waitUntil } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test, type SessionFactory } from "../fixtures.ts";
import { AgenticRpcClient, posix, runCli } from "./rpc.ts";

/// Start server with LSP+agentic, compile a file, wait for indexing.
async function indexedServer(
    session: SessionFactory,
): Promise<{ host: string; port: number; workspace: Workspace }> {
    const host = "127.0.0.1";
    const port = await findFreePort();
    // The first agentic query triggers a catch-up indexing round for open
    // files; the default 3s idle timer would dominate every fixture setup.
    const { client, workspace } = await session("index_features", {
        args: ["serve", "--host", host, "--port", String(port)],
        initializationOptions: { project: { idle_timeout_ms: 10 } },
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
    rpc.close();

    return { host, port, workspace };
}

test("cli compile command", async ({ session }) => {
    const { host, port, workspace } = await indexedServer(session);
    const p = posix(workspace.path("main.cpp"));
    const r = await runCli(cliceExecutable(), host, port, "compileCommand", { path: p });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { file: string; arguments: string[] };
    expect(data.file).toBe(p);
    expect(data.arguments.length).toBeGreaterThan(0);
});

test("cli symbol search", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "symbolSearch", { query: "add" });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as {
        symbols: { name: string; kind: string; line: number }[];
    };
    const names = data.symbols.map((s) => s.name);
    expect(names).toContain("add");
    const addSym = data.symbols.find((s) => s.name === "add")!;
    expect(addSym.kind).toBe("Function");
    expect(addSym.line).toBe(19);
});

test("cli definition", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "definition", { name: "add" });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as {
        name: string;
        definition: { startLine: number; endLine: number; text: string };
    };
    expect(data.name).toBe("add");
    const defn = data.definition;
    expect(defn.startLine).toBe(19);
    expect(defn.endLine).toBe(21);
    expect(defn.text).toContain("return a + b;");
});

test("cli definition by position", async ({ session }) => {
    const { host, port, workspace } = await indexedServer(session);
    const p = posix(workspace.path("main.cpp"));
    const r = await runCli(cliceExecutable(), host, port, "definition", { path: p, line: 19 });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { name: string };
    expect(data.name).toBe("add");
});

test("cli references", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "references", { name: "global_var" });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as {
        name: string;
        total: number;
        references: { line: number }[];
    };
    expect(data.name).toBe("global_var");
    expect(data.total).toBe(2);
    const lines = data.references.map((ref) => ref.line).sort((a, b) => a - b);
    expect(lines).toEqual([34, 38]);
});

test("cli read symbol", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "readSymbol", { name: "compute" });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { name: string; text: string };
    expect(data.name).toBe("compute");
    expect(data.text).toContain("add(1, 2)");
});

test("cli document symbols", async ({ session }) => {
    const { host, port, workspace } = await indexedServer(session);
    const p = posix(workspace.path("main.cpp"));
    const r = await runCli(cliceExecutable(), host, port, "documentSymbols", { path: p });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { symbols: { name: string }[] };
    const names = data.symbols.map((s) => s.name);
    expect(names).toContain("add");
    expect(names).toContain("main");
    expect(names).toContain("global_var");
});

test("cli call graph", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "callGraph", {
        name: "add",
        direction: "callers",
    });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { root: { name: string }; callers: { name: string }[] };
    expect(data.root.name).toBe("add");
    const callerNames = data.callers.map((c) => c.name);
    expect(callerNames).toContain("compute");
});

test("cli type hierarchy", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "typeHierarchy", {
        name: "Dog",
        direction: "supertypes",
    });
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { root: { name: string }; supertypes: { name: string }[] };
    expect(data.root.name).toBe("Dog");
    const supertypeNames = data.supertypes.map((t) => t.name);
    expect(supertypeNames).toContain("Animal");
});

test("cli project files", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "projectFiles");
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as { total: number; files: { path: string }[] };
    expect(data.total).toBeGreaterThan(0);
    const paths = data.files.map((f) => f.path);
    expect(paths.some((p) => p.includes("main.cpp"))).toBe(true);
});

test("cli status", async ({ session }) => {
    const { host, port } = await indexedServer(session);
    const r = await runCli(cliceExecutable(), host, port, "status");
    expect(r.status, `stderr: ${r.stderr}`).toBe(0);
    const data = JSON.parse(r.stdout) as {
        idle: boolean;
        total: number;
        pending: number;
        indexed: number;
    };
    expect(typeof data.idle).toBe("boolean");
    // total/pending describe the in-flight indexing round; the queue is
    // legitimately empty once the fixture's wait let the round drain.
    expect(data.total).toBeGreaterThanOrEqual(0);
    expect(Number.isInteger(data.pending)).toBe(true);
    expect(Number.isInteger(data.indexed)).toBe(true);
});

test("socket mode connects", async ({ session }) => {
    const port = await findFreePort();
    // In socket mode the LSP transport rides a TCP connection; a successful
    // session setup (connect + initialize) and teardown (clean shutdown)
    // are the assertion.
    const { client } = await session("hello_world", {
        args: ["serve", "--mode", "socket", "--port", String(port)],
        socketPort: port,
    });
    expect(client.initResult).not.toBeNull();
});
