import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

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

/// Whether a process has the pid: signal 0 checks without sending, and
/// ESRCH says none has.
function alive(pid: number): boolean {
    try {
        process.kill(pid, 0);
        return true;
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code === "ESRCH") {
            return false;
        }
        throw error;
    }
}

// Neovim quits by closing the server's input, without `exit`.
test("closed input ends it", async (t) => {
    const s = await hung(t);
    // The editor's side of the pipe, which no action models.
    s.client.child.stdin.end();
    await s.client.assertExitedCleanly(15_000);
    s.client.dispose();
});

test("workers die with it", async (t) => {
    const s = await hung(t);
    const { workers } = await s.stats();
    expect(workers).toHaveLength(2);
    expect(workers.filter(alive)).toEqual(workers);
    await s.kill();
    // No event marks another process gone. Each worker watches its server
    // and exits with it (exit_with_parent): what is left is the time a
    // dying process takes to leave.
    await t.elapse(2_000, "workers exiting after their server");
    expect(workers.filter(alive)).toEqual([]);
});
