import { spawn } from "node:child_process";
import { once } from "node:events";
import * as net from "node:net";
import { test } from "@clice/tools/testing";
import { cliceExecutable } from "@clice/tools/session";

// A socket server takes a SIGTERM with an editor connected as a pipe server
// does: the connection ends with the server, which exits at once.

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

test(
    "SIGTERM with an editor connected",
    { except: { win32: "Windows has no SIGTERM to send a process" } },
    async (t) => {
        const port = await freePort();
        const server = spawn(cliceExecutable(), ["serve", "--mode=socket", `--port=${port}`], {
            cwd: t.root,
            stdio: ["ignore", "ignore", "pipe"],
        });
        const exited = once(server, "exit");
        let log = "";
        await new Promise<void>((resolve) => {
            server.stderr.on("data", (chunk: Buffer) => {
                log += chunk.toString();
                if (log.includes("Listening on")) {
                    resolve();
                }
            });
        });
        const editor = net.connect(port, "127.0.0.1");
        await once(editor, "connect");
        // Answered, the connection is accepted and its peer runs.
        editor.write(
            frame({
                jsonrpc: "2.0",
                id: 0,
                method: "initialize",
                params: { processId: null, rootUri: null, capabilities: {} },
            }),
        );
        await once(editor, "data");
        server.kill("SIGTERM");
        const [code, signal] = (await exited) as [number | null, NodeJS.Signals | null];
        editor.destroy();
        await t.show("exit", { code: code ?? -1, signal: signal ?? "" });
    },
);
