/// Tests for the perf-line parser (tools/bench/perf.ts) that turns
/// `[perf:<topic>] key=value` log lines into series and Chrome traces.

import { expect, test } from "vitest";
import { computeStats, parsePerfLines, summarize, toChromeTrace } from "@clice/tools/bench/perf";

const LINE =
    "[2026-08-14 10:00:01.500] [info] [thread 7] [indexer.cpp:793] " +
    "[perf:index] progress=3/10 file=/w/a.cpp bytes=1024 index_ms=12.5 merge_ms=3";

test("perf line parsing", () => {
    const events = parsePerfLines(`noise\n${LINE}\nmore noise\n`);
    expect(events).toHaveLength(1);
    const event = events[0]!;
    expect(event.topic).toBe("index");
    expect(event.ts).toBe(Date.parse("2026-08-14T10:00:01.500"));
    expect(event.values).toEqual({
        progress: "3/10",
        file: "/w/a.cpp",
        bytes: 1024,
        index_ms: 12.5,
        merge_ms: 3,
    });
});

test("perf line without timestamp", () => {
    const events = parsePerfLines("[perf:startup] phase=cdb_load entries=42 elapsed_ms=7\n");
    expect(events).toHaveLength(1);
    expect(events[0]!.ts).toBeNull();
    expect(events[0]!.values["elapsed_ms"]).toBe(7);
});

test("values may contain spaces", () => {
    const events = parsePerfLines(
        "[perf:index_query] kind=search query=foo bar results=2 elapsed_ms=0.42\n",
    );
    expect(events[0]!.values["query"]).toBe("foo bar");
    expect(events[0]!.values["results"]).toBe(2);
});

test("summarize discriminates by kind and keeps only durations", () => {
    const summary = summarize(
        parsePerfLines(
            [
                "[perf:build] kind=pch file=/w/a.h compile_ms=100 total_ms=120",
                "[perf:build] kind=pch file=/w/b.h compile_ms=300 total_ms=340",
                "[perf:build] kind=index file=/w/a.cpp compile_ms=50 total_ms=60",
                "[perf:startup] phase=dep_scan elapsed_ms=25",
                "[perf:index_detail] op=serialize copy_ms=17.4 pack_ms=155.3",
                "[perf:index_detail] op=build scope=full semantics_ms=80 finish_ms=5",
                "[perf:index_detail] op=build scope=interested semantics_ms=8 finish_ms=1",
                "[perf:index_query] kind=relations rel=Definition path=/w/a.cpp elapsed_ms=3",
                "[perf:index_query] kind=relations rel=Reference path=/w/a.cpp elapsed_ms=7",
            ].join("\n"),
        ),
    );
    expect(summary["index_detail.serialize.pack_ms"]?.p50).toBe(155.3);
    expect(summary["build.pch.compile_ms"]?.count).toBe(2);
    expect(summary["build.pch.compile_ms"]?.max).toBe(300);
    expect(summary["build.index.total_ms"]?.count).toBe(1);
    expect(summary["startup.dep_scan.elapsed_ms"]?.p50).toBe(25);
    // The secondary scope/rel discriminators keep materially different
    // operations in separate series.
    expect(summary["index_detail.build.full.semantics_ms"]?.p50).toBe(80);
    expect(summary["index_detail.build.interested.semantics_ms"]?.p50).toBe(8);
    expect(summary["index_query.relations.Definition.elapsed_ms"]?.p50).toBe(3);
    expect(summary["index_query.relations.Reference.elapsed_ms"]?.p50).toBe(7);
    // Non-duration keys (file=, kind=) never become series.
    expect(Object.keys(summary).every((name) => name.endsWith("_ms"))).toBe(true);
});

test("stats percentiles", () => {
    const stats = computeStats([5, 1, 3, 2, 4])!;
    expect(stats.count).toBe(5);
    expect(stats.min).toBe(1);
    expect(stats.p50).toBe(3);
    expect(stats.max).toBe(5);
    expect(stats.mean).toBe(3);
    expect(computeStats([])).toBeNull();
});

test("trace lanes preserve process and thread identity", () => {
    const master = parsePerfLines(
        "[2026-08-14 10:00:00.000] [info] [thread 1] [f:1] [perf:request] kind=Hover total_ms=4\n",
        1,
    );
    const worker = parsePerfLines(
        "[2026-08-14 10:00:00.050] [info] [thread 9] [f:2] [perf:query] kind=Hover total_ms=2\n",
        2,
    );
    const trace = JSON.parse(toChromeTrace([...master, ...worker], { 2: "worker.log" })) as {
        traceEvents: { ph: string; pid: number; tid: number; args: { name?: string } }[];
    };
    const meta = trace.traceEvents.find((e) => e.ph === "M")!;
    expect(meta.pid).toBe(2);
    expect(meta.args.name).toBe("worker.log");
    const spans = trace.traceEvents.filter((e) => e.ph === "X");
    expect(spans.map((e) => [e.pid, e.tid])).toEqual([
        [1, 1],
        [2, 9],
    ]);
});

test("chrome trace reconstructs spans", () => {
    const events = parsePerfLines(
        [
            "[2026-08-14 10:00:00.000] [info] [t] [f:1] [perf:startup] phase=dep_scan elapsed_ms=40",
            // No total_ms/elapsed_ms duration — not representable as a span.
            "[2026-08-14 10:00:00.100] [info] [t] [f:2] [perf:cache] ns=pch event=hit key=k",
            // No timestamp — cannot be placed on the timeline.
            "[perf:startup] phase=index_load elapsed_ms=5",
        ].join("\n"),
    );
    const trace = JSON.parse(toChromeTrace(events)) as {
        traceEvents: { name: string; ph: string; ts: number; dur: number }[];
    };
    expect(trace.traceEvents).toHaveLength(1);
    const span = trace.traceEvents[0]!;
    expect(span.name).toBe("startup.dep_scan");
    expect(span.ph).toBe("X");
    // Stamped at span end: 40ms duration ending at the base timestamp, in
    // microseconds relative to the earliest stamped event.
    expect(span.ts).toBe(-40_000);
    expect(span.dur).toBe(40_000);
});
