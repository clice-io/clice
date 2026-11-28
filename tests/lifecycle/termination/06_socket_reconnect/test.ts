import { spawn } from "node:child_process";
import { once } from "node:events";
import * as net from "node:net";
import { test } from "@clice/tools/testing";
import { withoutUserSettings } from "@clice/tools/process_gate";
import { cliceExecutable } from "@clice/tools/session";

// A socket server outlives its editor's connection: an editor connecting
// again is served — it cannot initialize the server a second time, which
// keeps the first editor's folders, but it can shut it down.

/// A TCP port nothing listens on, as the system hands one out.
async function freePort(): Promise<number> {
    const probe = net.createServer();
    probe.listen(0, "127.0.0.1");
    await once(probe, "listening");
    const { port } = probe.address() as net.AddressInfo;
    probe.close();
    await once(probe, "close");
    return port;
}

/// An LSP message as it goes over the wire.
function frame(message: object): string {
    const body = JSON.stringify(message);
    return `Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`;
}

/// The body of the first message `editor` receives.
async function reply(editor: net.Socket): Promise<unknown> {
    let text = "";
    for (;;) {
        const [chunk] = (await once(editor, "data")) as [Buffer];
        text += chunk.toString();
        const header = /Content-Length: (\d+)\r\n\r\n/.exec(text);
        if (header !== null) {
            const start = header.index + header[0].length;
            const length = Number(header[1]);
            if (Buffer.byteLength(text.slice(start)) >= length) {
                return JSON.parse(text.slice(start, start + length)) as unknown;
            }
        }
    }
}

/// A connection to the server at `port` once it listens, as an editor that
/// knows the port makes one: refused until then. Its stderr says nothing
/// in time: on Windows its first lines may wait there until it writes more.
async function connect(
    port: number,
    exited: Promise<unknown>,
    log: () => string,
): Promise<net.Socket> {
    for (;;) {
        const editor = net.connect(port, "127.0.0.1");
        const outcome = await Promise.race([
            once(editor, "connect").then(
                () => "connected",
                () => "refused",
            ),
            exited.then(() => "exited"),
        ]);
        if (outcome === "connected") {
            return editor;
        }
        editor.destroy();
        if (outcome === "exited") {
            throw new Error(`the server exited before it listened:\n${log()}`);
        }
        // No event: a port that starts listening tells no client.
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
}

test("an editor connecting again shuts the server down", async (t) => {
    const port = await freePort();
    const server = spawn(cliceExecutable(), ["serve", "--mode=socket", `--port=${port}`], {
        cwd: t.root,
        env: withoutUserSettings(process.env),
        stdio: ["ignore", "ignore", "pipe"],
    });
    const exited = once(server, "exit");
    let log = "";
    server.stderr.on("data", (chunk: Buffer) => (log += chunk.toString()));
    /// Once the server logged `line`.
    const logged = async (line: string): Promise<void> => {
        while (!log.includes(line)) {
            const ended = await Promise.race([
                once(server.stderr, "data").then(() => false),
                exited.then(() => true),
            ]);
            if (ended) {
                throw new Error(`the server exited before it logged ${line}:\n${log}`);
            }
        }
    };
    const first = await connect(port, exited, () => log);
    first.write(
        frame({
            jsonrpc: "2.0",
            id: 0,
            method: "initialize",
            params: { processId: null, rootUri: null, capabilities: {} },
        }),
    );
    await reply(first);
    first.destroy();
    await logged("Client disconnected");
    const second = await connect(port, exited, () => log);
    second.write(frame({ jsonrpc: "2.0", id: 1, method: "shutdown", params: null }));
    await t.show("shutdown", JSON.stringify(await reply(second)));
    second.write(frame({ jsonrpc: "2.0", method: "exit", params: null }));
    const [code, signal] = (await exited) as [number | null, NodeJS.Signals | null];
    second.destroy();
    await t.show("exit", { code: code ?? -1, signal: signal ?? "" });
});
