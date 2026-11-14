import { test } from "@clice/tools/testing";

const SILENT = "the clice server \\(pid \\d+\\) does not answer";

// A server holding the lock that does not answer — stopped, here, as a hung
// one would be — is given a few seconds: then `--fresh` answers from the
// index as it was saved, and `clice index` gives up.
test(
    "a stopped server",
    { except: { win32: "Windows has no SIGSTOP to send a process" }, errors: [SILENT] },
    async (t) => {
        const s = await t.start();
        await s.indexed();
        await t.cli("index");
        const pid = s.client.child.pid ?? 0;
        process.kill(pid, "SIGSTOP");
        try {
            const env = { CLICE_TEST_CONTROL_WAIT_MS: "500" };
            await t.cli("query symbol-search compute --fresh", { env });
            await t.cli("index", { env });
        } finally {
            process.kill(pid, "SIGCONT");
        }
    },
);
