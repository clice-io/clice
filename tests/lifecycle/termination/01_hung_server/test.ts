import { expect } from "vitest";
import { running, test, type Serve, type TestContext } from "@clice/tools/testing";

// A server whose worker is deep in a compile that never ends still ends
// when its editor goes: on its input closing, and with the workers going
// along when the server itself is killed.

/// A server whose stateful worker runs the endless compile.
async function hung(t: TestContext): Promise<Serve> {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("main.cpp", { pull: false });
    void s.hover(t.at("main.cpp", "constexpr long |x")).catch(() => undefined);
    await park.reached();
    await park.release();
    return s;
}

// Neovim quits by closing the server's input, without `exit`.
test("closed input ends it", async (t) => {
    const s = await hung(t);
    // The editor's side of the pipe, which no action models.
    s.client.child.stdin.end();
    await s.client.assertExitedCleanly();
    s.client.dispose();
});

test("workers die with it", async (t) => {
    const s = await hung(t);
    const { stateful, stateless } = (await s.stats()).workers;
    const workers = [...stateful, ...stateless];
    expect(workers).toHaveLength(2);
    expect(workers.filter(running)).toEqual(workers);
    await s.kill();
    // Each worker watches its server and exits with it (exit_with_parent).
    await t.gone(workers);
});
