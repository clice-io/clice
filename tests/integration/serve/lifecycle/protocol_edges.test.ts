/// Document-sync protocol edges: notifications that arrive outside the
/// expected lifecycle window, and replay of state that materialized before the
/// client handshake completed. A case that drives a server before its
/// handshake stops the case's server, which holds the cache, and spawns its
/// own on the workspace.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import type { CliceClient } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

const TEST_TOML =
    '[project]\ncache_dir = "${workspace}/.clice"\nenable_indexing = false\n' +
    "\n[tracker]\nworkspace_poll_seconds = 0\n";

const test = serve.data("hello_world");

function messageText(d: proto.Diagnostic): string {
    return typeof d.message === "string" ? d.message : d.message.value;
}

test("open before initialize", async ({ s }) => {
    await s.stop();
    const client = s.session.spawn(s.workspace);
    // didOpen racing ahead of the handshake is accepted; the session must be
    // fully usable once the server becomes ready. Register the waiter before
    // the handshake so a push emitted during it cannot be missed.
    const [uri] = client.open(s.workspace.path("main.cpp"));
    const arrived = client.armDiagnostics(uri);
    await client.initialize(s.workspace);

    const hover = await client.hoverAt(uri, 2, 4);
    expect(hover).not.toBeNull();
    expect(hover!.contents).not.toBeNull();
    await arrived;
    expect(client.errors(uri)).toEqual([]);
});

/// One file under two names: real/main.cpp and link/main.cpp through a
/// symlink, which Windows grants only with privileges.
function linked(name: string, body: (s: Serve) => Promise<void>): void {
    if (process.platform === "win32") {
        return;
    }
    serve.files({ "real/main.cpp": "int main() { return 0; }\n" })(name, async ({ s }) => {
        fs.symlinkSync(s.workspace.path("real"), s.workspace.path("link"));
        await body(s);
    });
}

linked("second name for an open file", async (s) => {
    // One file, one buffer: a document naming an open file through a
    // symlink shares the first document's answers while their texts agree,
    // gets none once they diverge, never edits the first document's
    // buffer, and closing it leaves nothing to take over after it.
    await s.compiled("real/main.cpp");
    s.open("link/main.cpp");
    await s.diagnostics("link/main.cpp");
    expect(s.client.diagnostics.get(s.uri("link/main.cpp")), "the shared push").toEqual([]);
    const second = at("link/main.cpp", "int m|ain");
    expect(await s.hover(second), "equal texts share answers").not.toBeNull();

    s.edit("link/main.cpp", { replace: "return 0", with: "return undefined_name" });
    const warnings = await s.diagnostics("link/main.cpp");
    expect(warnings.map(messageText)).toEqual([expect.stringContaining("also open as")]);
    expect(warnings[0]?.range, "at the document's start").toEqual({
        start: { line: 0, character: 0 },
        end: { line: 0, character: 0 },
    });
    await expect(s.hover(second)).rejects.toThrow("Document changed");
    expect(await s.references(second), "no rows answer for other text").toEqual([]);
    s.close("link/main.cpp");
    // An edit folded into the first buffer would recompile it on this pull.
    expect(
        await s.hover(at("real/main.cpp", "int m|ain")),
        "the first document stays open",
    ).not.toBeNull();
    expect(s.client.diagnostics.get(s.uri("link/main.cpp")), "its warning leaves with it").toEqual(
        [],
    );
    expect(
        await s.errors("real/main.cpp"),
        "the first document's buffer must be untouched",
    ).toEqual([]);
    s.close("real/main.cpp");
    await s.sync();
    expect((await s.client.stats()).sessions, "the closed second name stays closed").toBe(0);
});

linked("second name takes over on close", async (s) => {
    await s.compiled("real/main.cpp");
    s.open("link/main.cpp");
    s.edit("link/main.cpp", { replace: "return 0", with: "return undefined_name" });
    s.close("real/main.cpp");
    expect(
        (await s.errors("link/main.cpp")).length,
        "the second document compiles with its own edits",
    ).toBeGreaterThan(0);
});

test("close before initialize", async ({ s }) => {
    await s.stop();
    const client = s.session.spawn(s.workspace);
    const [uri] = client.open(s.workspace.path("main.cpp"));
    client.close(uri);
    await client.initialize(s.workspace);
    // The pre-handshake close must not push a diagnostics clear (an ungated one
    // would be on the wire before the initialize response), and the closed
    // session must not be replayed.
    expect(client.diagnostics.has(uri)).toBe(false);
    await expect(client.hoverAt(uri, 0, 0)).rejects.toThrow("Document not open");
    // The file closed before ready went through the reindex queue; a normal
    // open/compile cycle must still work afterwards.
    const [uri2] = await client.openAndWait("main.cpp");
    expect(client.errors(uri2)).toEqual([]);
});

test("change without open", async ({ s }) => {
    // No didOpen baseline: the edit must be dropped.
    s.client.change(s.uri("main.cpp"), 1, "int broken(");
    await expect(s.hover(at("main.cpp", "|#include"))).rejects.toThrow("Document not open");
    // The dropped edit must not poison a later open.
    expect(await s.errors("main.cpp")).toEqual([]);
});

serve.files({ "main.cpp": "int foo() { return 1; }\n" })("desync range clamped", async ({ s }) => {
    await s.compiled("main.cpp");

    // An incremental edit whose range lies outside the buffer: the views have
    // drifted. The range is clamped per LSP 3.17, so "oops" lands at the true
    // end of the document instead of being dropped.
    const outside = { start: { line: 999, character: 0 }, end: { line: 999, character: 5 } };
    s.client.changeRange(s.uri("main.cpp"), 2, outside, "oops");

    // Requests keep being served, now against the clamped buffer.
    expect(await s.hover(at("main.cpp", "int |foo"))).not.toBeNull();

    // The appended "oops" makes the TU ill-formed: errors prove the edit was
    // applied rather than dropped.
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);

    await s.stop();
    const clamped = s.workspace
        .log("master.log")
        .split("\n")
        .some((line) => line.includes("didChange range") && line.includes("clamped"));
    expect(clamped, "clamped out-of-sync edit never produced a clamp log").toBe(true);
});

test("version regression tolerated", async ({ s }) => {
    const [uri, content] = s.client.open("main.cpp", 5);
    // A version that goes backwards is a client bug; the edit is applied anyway
    // (and warned about server-side).
    s.client.change(uri, 3, content + "\nint bad(\n");
    await s.hover(at("main.cpp", "|#include"));
    const errors = (await s.client.pullDiagnostics(uri)).filter(
        (d) => d.severity === proto.DiagnosticSeverity.Error,
    );
    expect(errors.length).toBeGreaterThan(0);
});

/// A server pre-initialized on the case's workspace (serve --workspace),
/// whose client has not done its handshake.
async function preInitialized(s: Serve): Promise<CliceClient> {
    await s.stop();
    return s.session.spawn(s.workspace, { args: ["serve", `--workspace=${s.workspace.root}`] });
}

const late = serve.files({
    "main.cpp": "int add(int a, int b) { return a + b; }\n",
    "clice.toml": TEST_TOML,
});

late("replay after late handshake", async ({ s }) => {
    const client = await preInitialized(s);
    // The server is pre-initialized (ready); the client has not done its
    // handshake yet. Compile output materializes but must not be pushed.
    const [uri] = client.open(s.workspace.path("main.cpp"));
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
        client.sendRequest(proto.InitializeRequest.type, {
            processId: null,
            rootUri: s.workspace.uri(),
            capabilities: {},
        }),
    ).rejects.toThrow();
    const arrived = client.armDiagnostics(uri);
    await client.sendNotification(proto.InitializedNotification.type, {});
    await arrived;
    expect(client.errors(uri)).toEqual([]);
});

late("no stale replay", async ({ s }) => {
    const client = await preInitialized(s);
    const [uri, content] = client.open(s.workspace.path("main.cpp"));
    const hover = await client.hoverAt(uri, 0, 4);
    expect(hover).not.toBeNull();
    // An edit during the handshake window invalidates the materialized
    // output; the replay must skip it instead of pairing pre-edit
    // results with the new text.
    client.change(uri, 1, content + "int bad(\n");
    await expect(
        client.sendRequest(proto.InitializeRequest.type, {
            processId: null,
            rootUri: s.workspace.uri(),
            capabilities: {},
        }),
    ).rejects.toThrow();
    await client.sendNotification(proto.InitializedNotification.type, {});
    // A request round-trip orders us after the initialized processing: a
    // (wrong) replay push would already have been recorded.
    await client.queryContext(uri);
    expect(client.diagnostics.has(uri)).toBe(false);
    // The next compile pushes fresh results for the edited buffer.
    const arrived = client.armDiagnostics(uri);
    await client.hoverAt(uri, 0, 4);
    await arrived;
    expect(client.errors(uri).length).toBeGreaterThan(0);
});

serve.files({ "main.cpp": "int x = 1;\n", "clice.toml": TEST_TOML })(
    "startup guidance delivered",
    async ({ s }) => {
        // No compile_commands.json: the headless workspace load emits
        // guidance without waiting for any handshake; the client must still
        // receive it (drained from the server's notify log).
        s.disk.rm("compile_commands.json");
        const client = await preInitialized(s);
        // The load runs before the server reads its first message, so the
        // guidance is on the wire ahead of the shutdown reply.
        await client.shutdown();
        expect(
            client.guidanceMessages().some((message) => message.includes("compile_commands.json")),
            "startup guidance never reached the client",
        ).toBe(true);
    },
);
