/// How a server ends when its client goes away abruptly: the client closes
/// the server's input, kills it, or dies itself.

import { spawn } from "node:child_process";
import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { runProcess } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

const hanging = serve
    .files(
        {
            "hang.cpp":
                "constexpr long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n" +
                "constexpr long x = fib(90);\n",
        },
        {
            config: { project: { enable_indexing: false } },
            manifest: {
                cxx: ["-std=c++23"],
                units: { "hang.cpp": ["-fconstexpr-steps=2147483647"] },
            },
        },
    )
    .skipIf(process.platform !== "linux");

/// Open the hanging file and ask for a hover: the stateful worker runs its
/// compile, which never ends. What the server still runs at the deadline
/// of its own sync is in flight.
async function hang(s: Serve): Promise<void> {
    s.open("hang.cpp");
    void s.hover(at("hang.cpp", "long f|ib(")).catch(() => undefined);
    const { pending } = await s.client.sync({ deadlineMs: 1_000 });
    expect(pending).toContain(`compile ${s.workspace.displayPath("hang.cpp")}`);
}

/// Waits for process `pid`, which is no child of this one, to exit: a
/// pidfd of it turns readable then. Python's os.pidfd_open is missing from
/// some builds; 434 is pidfd_open's number on every Linux architecture.
async function exited(pid: number): Promise<void> {
    const wait = [
        "import ctypes, select, sys",
        "fd = ctypes.CDLL(None, use_errno=True).syscall(434, int(sys.argv[1]), 0)",
        "if fd < 0 and ctypes.get_errno() != 3:",
        "    sys.exit(f'pidfd_open: errno {ctypes.get_errno()}')",
        "if fd >= 0:",
        "    select.select([fd], [], [])",
    ].join("\n");
    const run = await runProcess("python3", ["-c", wait, String(pid)], { timeout: 10_000 });
    expect(run.status, `worker ${pid} outlived its master: ${run.stderr}`).toBe(0);
}

hanging("closed input ends a hung server", async ({ s }) => {
    await hang(s);
    // Neovim quits by closing the server's input, without `exit`.
    s.client.child.stdin.end();
    await s.client.assertExitedCleanly(15_000);
    s.client.dispose();
});

hanging("workers die with their master", async ({ s }) => {
    await hang(s);
    const workers = s.client.workerPids();
    await s.kill();
    await Promise.all(workers.map(exited));
});

/// The editor a server reports to, standing in for one that dies while a
/// descendant keeps the server's input open.
serve
    .files({ "main.cpp": "int main() { return 0; }\n" }, { launch: { handshake: false } })
    .for([false, true])(
    "client death ends the server (after shutdown: %s)",
    async (shutdown, { s }) => {
        // The server watches the process initialize names as its editor: the
        // case initializes by hand to name its own.
        const editor = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"]);
        try {
            await s.client.sendRequest("initialize", {
                processId: editor.pid,
                rootUri: s.workspace.uri(),
                capabilities: {},
                initializationOptions: { project: { cache_dir: s.workspace.path(".clice") } },
            });
            await s.client.sendNotification(proto.InitializedNotification.type, {});
            if (shutdown) {
                await s.client.sendRequest(proto.ShutdownRequest.type);
            }
        } finally {
            editor.kill("SIGKILL");
        }
        await s.client.assertExitedCleanly(15_000);
        s.client.dispose();
    },
);
