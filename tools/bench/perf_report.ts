/// Offline report over clice's `[perf:*]` log lines: aggregate a real
/// run's master/worker logs into per-series latency stats, optionally
/// exporting a Chrome trace for Perfetto.
///
/// Usage:
///   node tools/bench/perf_report.ts <logfile>... [--trace out.json]
///
/// Log files live under the server's logging dir (master.log and the
/// per-worker logs); any text with perf lines works, including captured
/// stderr.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { parsePerfLines, summarize, toChromeTrace, type PerfEvent } from "./perf.ts";

const { values, positionals } = parseArgs({
    options: {
        trace: { type: "string" },
        help: { type: "boolean", default: false },
    },
    allowPositionals: true,
});

if (values.help || positionals.length === 0) {
    console.log("Usage: node tools/bench/perf_report.ts <logfile>... [--trace out.json]");
    process.exit(values.help ? 0 : 1);
}

// One trace pid per input log: master and workers are separate processes,
// and their concurrent activity must land on separate lanes.
const events: PerfEvent[] = [];
const processNames: Record<number, string> = {};
positionals.forEach((file, index) => {
    const pid = index + 1;
    processNames[pid] = path.basename(file);
    // No spread into push: a long session yields enough events to blow
    // the engine's argument-count limit.
    for (const event of parsePerfLines(fs.readFileSync(file, "utf8"), pid)) {
        events.push(event);
    }
});

if (events.length === 0) {
    console.log("no [perf:*] lines found");
    process.exit(0);
}

console.log(`${events.length} perf events`);
console.log(
    `${"series".padEnd(44)} ${"count".padStart(6)} ${"p50".padStart(9)} ` +
        `${"p90".padStart(9)} ${"max".padStart(9)} ${"sum".padStart(10)}`,
);
const summary = Object.entries(summarize(events)).sort((a, b) => b[1].sum - a[1].sum);
for (const [series, stats] of summary) {
    console.log(
        `${series.padEnd(44)} ${String(stats.count).padStart(6)} ` +
            `${stats.p50.toFixed(2).padStart(9)} ${stats.p90.toFixed(2).padStart(9)} ` +
            `${stats.max.toFixed(2).padStart(9)} ${stats.sum.toFixed(0).padStart(10)}`,
    );
}

if (values.trace !== undefined) {
    fs.writeFileSync(values.trace, toChromeTrace(events, processNames));
    console.log(`\nChrome trace written to ${values.trace} (open in Perfetto)`);
}
