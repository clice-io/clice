/// Document-sync protocol edges: notifications that arrive outside the
/// expected lifecycle window, and replay of state that materialized before the
/// client handshake completed.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { assertNoAnomaly, getErrors, guidanceMessages, sleep } from "../../tools/checks.ts";
import { CliceClient, withTimeout } from "../../tools/client.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { shutdownClient } from "../../tools/lifecycle.ts";

const TEST_TOML =
    '[project]\ncache_dir = "${workspace}/.clice"\nenable_indexing = false\n' +
    "\n[tracker]\ncdb_poll_seconds = 0\nworkspace_poll_seconds = 0\n";

// hello_world's main.cpp, recreated in a temp workspace so the pre-handshake
// tests never touch the shared data workspace.
const HELLO_WORLD = `#include <iostream>

int add(int a, int b) {
    return a + b;
}

int main() {
    std::cout << "hello world" << std::endl;

    int result = add(1, 2);
    return result;
}
`;

test("open before initialize", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), HELLO_WORLD);
    writeCdb(workspace, ["main.cpp"]);

    // didOpen racing ahead of the handshake is accepted; the session must be
    // fully usable once the server becomes ready. Register the waiter before
    // the handshake so a push emitted during it cannot be missed.
    const [uri] = client.open(path.join(workspace, "main.cpp"));
    const arrived = client.armDiagnostics(uri);
    await client.initialize(workspace);

    const hover = await client.hoverAt(uri, 2, 4);
    expect(hover).not.toBeNull();
    expect(hover!.contents).not.toBeNull();
    await withTimeout(arrived, 60_000, "diagnostics");
    expect(getErrors(client.diagnostics.get(uri) ?? [])).toEqual([]);
});

test("close before initialize", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), HELLO_WORLD);
    writeCdb(workspace, ["main.cpp"]);

    const [uri] = client.open(path.join(workspace, "main.cpp"));
    client.close(uri);
    await client.initialize(workspace);
    // The pre-handshake close must not push a diagnostics clear (an ungated one
    // would be on the wire before the initialize response), and the closed
    // session must not be replayed.
    expect(client.diagnostics.has(uri)).toBe(false);
    await expect(client.hoverAt(uri, 0, 0)).rejects.toThrow("Document not open");
    // The file closed before ready went through the reindex queue; a normal
    // open/compile cycle must still work afterwards.
    const [uri2] = await client.openAndWait(path.join(workspace, "main.cpp"));
    expect(getErrors(client.diagnostics.get(uri2) ?? [])).toEqual([]);
});

test("change without open", async ({ session }) => {
    const { client, workspace } = await session("hello_world");
    const uri = URI.file(path.join(workspace, "main.cpp")).toString();
    // No didOpen baseline: the edit must be dropped.
    client.change(uri, 1, "int broken(");
    await expect(client.hoverAt(uri, 0, 0)).rejects.toThrow("Document not open");
    // The dropped edit must not poison a later open.
    const [uri2] = await client.openAndWait(path.join(workspace, "main.cpp"));
    expect(getErrors(client.diagnostics.get(uri2) ?? [])).toEqual([]);
});

test("desync range clamped", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int foo() { return 1; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));

    // An incremental edit whose range lies outside the buffer: the views have
    // drifted. The range is clamped per LSP 3.17, so "oops" lands at the true
    // end of the document instead of being dropped.
    const arrived = client.armDiagnostics(uri);
    await client.connection.sendNotification(proto.DidChangeTextDocumentNotification.type, {
        textDocument: { uri, version: 2 },
        contentChanges: [
            {
                range: {
                    start: { line: 999, character: 0 },
                    end: { line: 999, character: 5 },
                },
                text: "oops",
            },
        ],
    });

    // Requests keep being served, now against the clamped buffer.
    const hover = await client.hoverAt(uri, 0, 4);
    expect(hover).not.toBeNull();

    // The appended "oops" makes the TU ill-formed: errors prove the edit was
    // applied rather than dropped.
    await withTimeout(arrived, 60_000, "diagnostics");
    expect(getErrors(client.diagnostics.get(uri) ?? []).length).toBeGreaterThan(0);

    const logsDir = path.join(workspace, ".clice", "logs");
    let clamped = false;
    for (let i = 0; i < 50; i++) {
        const logs = fs.existsSync(logsDir)
            ? fs
                  .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
                  .filter((name) => name.endsWith(".log"))
                  .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"))
                  .join("")
            : "";
        if (
            logs.split("\n").some((ln) => ln.includes("didChange range") && ln.includes("clamped"))
        ) {
            clamped = true;
            break;
        }
        await sleep(100);
    }
    expect(clamped, "clamped out-of-sync edit never produced a clamp log").toBe(true);
});

test("version regression tolerated", async ({ session }) => {
    const { client, workspace } = await session("hello_world");
    const [uri, content] = client.open(path.join(workspace, "main.cpp"), 5);
    // A version that goes backwards is a client bug; the edit is applied anyway
    // (and warned about server-side).
    const arrived = client.armDiagnostics(uri);
    client.change(uri, 3, content + "\nint bad(\n");
    await client.hoverAt(uri, 0, 0);
    await withTimeout(arrived, 60_000, "diagnostics");
    expect(getErrors(client.diagnostics.get(uri) ?? []).length).toBeGreaterThan(0);
});

function mkTmp(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
}

/// Spawn a pre-initialized server bound to `workspace` via --workspace, with
/// teardown that gates shutdown and anomalies and removes the temp workspace.
async function withLateHandshake(
    setup: (workspace: string) => void,
    body: (client: CliceClient, workspace: string) => Promise<void>,
): Promise<void> {
    const workspace = mkTmp();
    try {
        setup(workspace);
        const client = CliceClient.start(cliceExecutable(), {
            args: ["serve", `--workspace=${workspace}`],
        });
        try {
            await body(client, workspace);
        } finally {
            client.workspace = workspace;
            try {
                await shutdownClient(client);
            } finally {
                assertNoAnomaly(client, workspace);
            }
        }
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
}

test("replay after late handshake", async () => {
    await withLateHandshake(
        (workspace) => {
            fs.writeFileSync(
                path.join(workspace, "main.cpp"),
                "int add(int a, int b) { return a + b; }\n",
            );
            writeCdb(workspace, ["main.cpp"]);
            fs.writeFileSync(path.join(workspace, "clice.toml"), TEST_TOML);
        },
        async (client, workspace) => {
            // The server is pre-initialized (ready); the client has not done its
            // handshake yet. Compile output materializes but must not be pushed.
            const [uri] = client.open(path.join(workspace, "main.cpp"));
            const hover = await client.hoverAt(uri, 0, 4);
            expect(hover).not.toBeNull();
            // Non-vacuous: an ungated push is emitted during the compile the
            // hover awaits, so it would be on the wire before the hover response
            // and recorded by the time the hover future resolves.
            expect(client.diagnostics.has(uri)).toBe(false);

            // A pre-initialized server rejects the initialize request; the
            // handshake still completes with the initialized notification, which
            // replays the materialized output.
            await expect(
                client.connection.sendRequest(proto.InitializeRequest.type, {
                    processId: null,
                    rootUri: URI.file(workspace).toString(),
                    capabilities: {},
                }),
            ).rejects.toThrow();
            const arrived = client.armDiagnostics(uri);
            await client.connection.sendNotification(proto.InitializedNotification.type, {});
            await withTimeout(arrived, 30_000, "diagnostics");
            expect(getErrors(client.diagnostics.get(uri) ?? [])).toEqual([]);
        },
    );
});

test("no stale replay", async () => {
    await withLateHandshake(
        (workspace) => {
            fs.writeFileSync(
                path.join(workspace, "main.cpp"),
                "int add(int a, int b) { return a + b; }\n",
            );
            writeCdb(workspace, ["main.cpp"]);
            fs.writeFileSync(path.join(workspace, "clice.toml"), TEST_TOML);
        },
        async (client, workspace) => {
            const [uri, content] = client.open(path.join(workspace, "main.cpp"));
            const hover = await client.hoverAt(uri, 0, 4);
            expect(hover).not.toBeNull();
            // An edit during the handshake window invalidates the materialized
            // output; the replay must skip it instead of pairing pre-edit
            // results with the new text.
            client.change(uri, 1, content + "int bad(\n");
            await expect(
                client.connection.sendRequest(proto.InitializeRequest.type, {
                    processId: null,
                    rootUri: URI.file(workspace).toString(),
                    capabilities: {},
                }),
            ).rejects.toThrow();
            await client.connection.sendNotification(proto.InitializedNotification.type, {});
            // A request round-trip orders us after the initialized processing: a
            // (wrong) replay push would already have been recorded.
            await client.queryContext(uri);
            expect(client.diagnostics.has(uri)).toBe(false);
            // The next compile pushes fresh results for the edited buffer.
            const arrived = client.armDiagnostics(uri);
            await client.hoverAt(uri, 0, 4);
            await withTimeout(arrived, 30_000, "diagnostics");
            expect(getErrors(client.diagnostics.get(uri) ?? []).length).toBeGreaterThan(0);
        },
    );
});

test("startup guidance delivered", async () => {
    await withLateHandshake(
        (workspace) => {
            fs.writeFileSync(path.join(workspace, "main.cpp"), "int x = 1;\n");
            fs.writeFileSync(path.join(workspace, "clice.toml"), TEST_TOML);
            // No compile_commands.json: the headless workspace load emits
            // guidance without waiting for any handshake; the client must still
            // receive it (drained from the server's notify log).
        },
        async (client) => {
            let delivered = false;
            for (let i = 0; i < 300; i++) {
                if (guidanceMessages(client).some((m) => m.includes("compile_commands.json"))) {
                    delivered = true;
                    break;
                }
                await sleep(100);
            }
            expect(delivered, "startup guidance never reached the client").toBe(true);
        },
    );
});
