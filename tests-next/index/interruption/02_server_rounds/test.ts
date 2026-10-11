import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { generate, test, type Serve, type TestContext } from "@clice/tools/testing";

// A background round cut short, and what the next `clice index` finds on
// disk: it reindexes only what no save committed. Of the two units, the
// first one's index is let through and merged, the second one's is held at
// its reply, so the round cannot end and save on its own. The round waits
// out an idle delay longer than placing the holds takes. A kill saves
// nothing itself; a shutdown or SIGTERM lets the held reply through and
// saves both.
const IDLE = { project: { idle_timeout_ms: 1_000 } };

async function cutShort(t: TestContext, env: Record<string, string> = {}): Promise<Serve> {
    t.disk.write(generate.units(2));
    const s = await t.start({ config: IDLE, env });
    const first = await s.park("index", "unit_0.cpp", { at: "reply" });
    const second = await s.park("index", "unit_1.cpp", { at: "reply" });
    await first.reached();
    await first.release();
    await second.reached();
    return s;
}

test("checkpoint survives a kill", async (t) => {
    const s = await cutShort(t, { CLICE_TEST_CHECKPOINT_MS: "100" });
    await t.elapse(
        1_000,
        "checkpoints every 100 ms (CLICE_TEST_CHECKPOINT_MS) commit the merged unit",
    );
    await s.kill();
    await t.cli("index");
});

// Zed kills the server as soon as the shutdown request is answered.
test("shutdown reply follows the save", async (t) => {
    const s = await cutShort(t);
    // The raw client: the shutdown request alone, with no exit after it.
    await s.client.sendRequest(proto.ShutdownRequest.type);
    await s.kill();
    await t.cli("index");
});

test(
    "sigterm saves the round",
    { except: { win32: "Windows has no SIGTERM to send" } },
    async (t) => {
        const s = await cutShort(t);
        // The raw client: the signal is what the test sends, not a request.
        s.client.child.kill("SIGTERM");
        expect(await s.client.exited).toBe(0);
        s.client.dispose();
        await t.cli("index");
    },
);
