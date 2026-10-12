import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// What a worker crash leaves behind: the anomaly reaches the editor's log,
// and the crashed worker's own log holds a backtrace with what offline
// symbolization (scripts/symbolize.py) needs, the executable's load base
// and the version and target line. The master's log never holds it.

test(async (t) => {
    const s = await t.start({ crash: { request: "query:Hover", file: "main.cpp" } });
    s.open("main.cpp");
    await t.show("hover", s.hover(t.at("main.cpp", "int |add")));
    await t.show("crashes", await s.crashes());
    // The editor's output channel: window/logMessage, not modelled by the
    // actions.
    expect(s.client.anomaliesInLogMessages()).toEqual(["WorkerCrash"]);
    const logs = t.disk.list(".clice/logs/**/*.log");
    const reports = logs.filter((log) => t.disk.read(log).includes("CRASH STACK TRACE"));
    expect(reports.map((log) => path.basename(log))).toEqual(["SF-0.log"]);
    const report = t.disk.read(reports[0] ?? "");
    expect(report).toMatch(/main executable base: 0x[0-9a-fA-F]+/);
    expect(report).toMatch(/^clice \S+ \S+$/m);
    const master = logs.filter((log) => path.basename(log) === "master.log");
    expect(master.filter((log) => /Stack dump|CRASH STACK TRACE/.test(t.disk.read(log)))).toEqual(
        [],
    );
});
