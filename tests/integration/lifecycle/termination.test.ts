/// How a server ends when its client goes away abruptly: the client closes
/// the server's input, kills it, or dies itself.

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { waitUntil, type CliceClient } from "@clice/tools/client";
import type { Workspace } from "@clice/tools/workspace";
import { test, type SessionFactory } from "../fixtures.ts";

function hangingWorkspace(ws: Workspace): void {
    ws.write(
        "hang.cpp",
        "constexpr long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n" +
            "constexpr long x = fib(90);\n",
    );
    ws.writeCDB(["hang.cpp"], { extraArgs: ["-fconstexpr-steps=2147483647"] });
}

/// CPU time `pid` has used, in clock ticks.
function cpuTicks(pid: number): number {
    const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
    const fields = stat
        .slice(stat.lastIndexOf(")") + 1)
        .trim()
        .split(/\s+/);
    return Number(fields[11]) + Number(fields[12]);
}

function alive(pid: number): boolean {
    try {
        process.kill(pid, 0);
        return true;
    } catch {
        return false;
    }
}

/// Open the hanging file, ask for a hover, and wait until the stateful
/// worker is deep in its compile. Linux only: read from /proc.
async function hang(client: CliceClient): Promise<void> {
    const [uri] = client.open("hang.cpp");
    void client.hoverAt(uri, 0, 16).catch(() => undefined);
    await waitUntil(() => client.workerPids("SF-").some((pid) => cpuTicks(pid) > 100), {
        timeout: 60_000,
        interval: 100,
        description: "the stateful worker running the hung compile",
    });
}

async function hangingServer(session: SessionFactory): Promise<CliceClient> {
    const ws = session.tmpdir();
    hangingWorkspace(ws);
    const client = session.spawn(ws);
    await client.initialize(ws, {
        initializationOptions: { project: { enable_indexing: false } },
    });
    await hang(client);
    return client;
}

test.skipIf(process.platform !== "linux")(
    "closed input ends a hung server",
    async ({ session }) => {
        const client = await hangingServer(session);
        // Neovim quits by closing the server's input, without `exit`.
        client.child.stdin.end();
        await client.assertExitedCleanly(15_000);
        client.dispose();
    },
);

test.skipIf(process.platform !== "linux")("workers die with their master", async ({ session }) => {
    const client = await hangingServer(session);
    const workers = client.workerPids();
    client.killServer();
    client.dispose();
    await waitUntil(() => !workers.some(alive), {
        timeout: 10_000,
        interval: 100,
        description: "the workers of the killed master to exit",
    });
});

/// The editor a server reports to, standing in for one that dies while a
/// descendant keeps the server's input open.
test.for([false, true])(
    "client death ends the server (after shutdown: %s)",
    async (shutdown, { session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("main.cpp", "int main() { return 0; }\n");
        workspace.writeCDB(["main.cpp"]);
        const editor = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"]);
        try {
            await client.sendRequest("initialize", {
                processId: editor.pid,
                rootUri: workspace.uri(),
                capabilities: {},
                initializationOptions: { project: { cache_dir: workspace.path(".clice") } },
            });
            await client.sendNotification(proto.InitializedNotification.type, {});
            if (shutdown) {
                await client.sendRequest(proto.ShutdownRequest.type);
            }
        } finally {
            editor.kill("SIGKILL");
        }
        await client.assertExitedCleanly(15_000);
        client.dispose();
    },
);
