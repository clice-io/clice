import { setTimeout as sleep } from "node:timers/promises";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// The log keeps a line for each request that took over 200 ms or failed,
// with what it waited for; a quick one leaves its line at debug level, which
// the log does not keep.

/// The hover lines of the session's one master.log, after its server stopped.
function hovers(t: TestContext): string[] {
    const logs = t.disk.list(".clice/logs/*/master.log");
    expect(logs).toHaveLength(1);
    return t.disk
        .read(logs[0] ?? "")
        .split("\n")
        .filter((line) => line.includes("textDocument/hover on main.cpp"));
}

test("a request kept waiting for its compile", async (t) => {
    // Without an index to answer from, the hover waits for the compile.
    const s = await t.start({ lastRule: { background_index: false } });
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("main.cpp", { pull: false });
    const hover = s.hover(t.at("main.cpp", "return |add"));
    await park.reached();
    // Time is the feature: a request answered past 200 ms leaves a line.
    await sleep(400);
    await park.release();
    expect(await hover).not.toBeNull();
    // Compiled now: this one answers at once.
    expect(await s.hover(t.at("main.cpp", "int |add"))).not.toBeNull();
    await s.stop();
    const lines = hovers(t);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toMatch(
        /textDocument\/hover on main\.cpp answered in \d+ ms, \d+ ms of it waiting for its compile$/,
    );
});
