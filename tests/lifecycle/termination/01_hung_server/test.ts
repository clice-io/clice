import * as fs from "node:fs";
import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

// A server whose worker is deep in a compile that never ends still ends
// when its editor goes: on its input closing, and with the workers going
// along when the server itself is killed.

const LINUX = {
    except: {
        win32: "reads worker processes from /proc",
        darwin: "reads worker processes from /proc",
    },
};

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

function alive(pid: number): boolean {
    try {
        const stat = fs.readFileSync(`/proc/${String(pid)}/stat`, "utf8");
        return stat.slice(stat.lastIndexOf(")") + 2, stat.lastIndexOf(")") + 3) !== "Z";
    } catch {
        return false;
    }
}

// Neovim quits by closing the server's input, without `exit`.
test("closed input ends it", LINUX, async (t) => {
    const s = await hung(t);
    // The editor's side of the pipe, which no action models.
    s.client.child.stdin.end();
    await s.client.assertExitedCleanly(15_000);
    s.client.dispose();
});

test("workers die with it", LINUX, async (t) => {
    const s = await hung(t);
    const workers = s.client.workerPids();
    expect(workers).toHaveLength(2);
    await s.kill();
    // No event marks another process gone: the kernel's death signal to
    // the workers lands as the server exits, and they are reaped soon after.
    for (let tries = 0; workers.some(alive); tries++) {
        expect(tries, "the workers outlived their server").toBeLessThan(100);
        await t.elapse(100, "a killed process leaving /proc");
    }
});
