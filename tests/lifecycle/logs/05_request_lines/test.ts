import { test, type TestContext } from "@clice/tools/testing";

// The log keeps a line for each request that took over 200 ms or failed,
// with what it waited for; a quick one leaves its line at debug level, which
// the log does not keep.

/// The hover lines of the session's one master.log, after its server
/// stopped: their message, times elided.
function hovers(t: TestContext): string[] {
    const [log, ...others] = t.disk.list(".clice/logs/*/master.log");
    if (log === undefined || others.length > 0) {
        throw new Error("the session keeps one master.log");
    }
    return t.disk
        .read(log)
        .split("\n")
        .filter((line) => line.includes("textDocument/hover on main.cpp"))
        .map((line) => line.slice(line.indexOf("textDocument/hover")).replace(/\d+ ms/g, "# ms"));
}

test("a request kept waiting for its compile", async (t) => {
    // Without an index to answer from, the hover waits for the compile.
    const s = await t.start({ lastRule: { background_index: false } });
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.open("main.cpp", { pull: false });
    const hover = s.hover(t.at("main.cpp", "return |add"));
    await park.reached();
    await t.elapse(400, "a request answered past 200 ms leaves a line");
    await park.release();
    await t.show("waited", hover);
    // Compiled now: this one answers at once.
    await t.show("at once", s.hover(t.at("main.cpp", "int |add")));
    await s.stop();
    await t.show("logged", hovers(t));
});
