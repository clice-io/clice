/// Parser and aggregator for clice's `[perf:<topic>] key=value` log lines
/// (see src/support/logging.h). Consumed by bench.ts for the server-side
/// breakdown of a benchmark run and by perf_report.ts for offline log
/// analysis.

/// One `[perf:topic]` line: string keys kept verbatim, numeric values
/// parsed. `ts` is the log line's timestamp in epoch milliseconds when the
/// line carried one (stderr mirrors and log files do).
export interface PerfEvent {
    topic: string;
    ts: number | null;
    values: Record<string, string | number>;
}

/// `[2026-08-14 12:34:56.789] [info] [thread 1] [file.cpp:42] [perf:x] k=v`
const LINE_PATTERN =
    /^(?:\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\] )?.*?\[perf:([\w-]+)\] (.*)$/;

export function parsePerfLines(text: string): PerfEvent[] {
    const events: PerfEvent[] = [];
    for (const line of text.split("\n")) {
        const match = LINE_PATTERN.exec(line);
        if (match === null) {
            continue;
        }
        const [, stamp, topic, rest] = match;
        if (topic === undefined || rest === undefined) {
            continue;
        }
        const values: Record<string, string | number> = {};
        for (const pair of rest.split(" ")) {
            const eq = pair.indexOf("=");
            if (eq <= 0) {
                continue;
            }
            const key = pair.slice(0, eq);
            const raw = pair.slice(eq + 1);
            const num = Number(raw);
            values[key] = raw !== "" && Number.isFinite(num) ? num : raw;
        }
        events.push({
            topic,
            // The log stamp has no timezone; Date.parse reads it as local
            // time, which is what the producing process used.
            ts: stamp !== undefined ? Date.parse(stamp.replace(" ", "T")) : null,
            values,
        });
    }
    return events;
}

export interface Stats {
    count: number;
    min: number;
    p50: number;
    p90: number;
    p99: number;
    max: number;
    mean: number;
    sum: number;
}

export function computeStats(values: number[]): Stats | null {
    if (values.length === 0) {
        return null;
    }
    const sorted = [...values].sort((a, b) => a - b);
    const at = (p: number): number => sorted[Math.floor(p * (sorted.length - 1))] ?? 0;
    const sum = sorted.reduce((a, b) => a + b, 0);
    return {
        count: sorted.length,
        min: sorted[0] ?? 0,
        p50: at(0.5),
        p90: at(0.9),
        p99: at(0.99),
        max: sorted[sorted.length - 1] ?? 0,
        mean: sum / sorted.length,
        sum,
    };
}

/// Group events into duration series. Every numeric `*_ms` key becomes a
/// series named `<topic>[.<kind>].<key>`, where the kind discriminator is
/// the event's `kind` or `phase` value when present — mirroring how the
/// topics in logging.h use those fields.
export function aggregate(events: PerfEvent[]): Map<string, number[]> {
    const series = new Map<string, number[]>();
    for (const event of events) {
        const kind = event.values["kind"] ?? event.values["phase"];
        const prefix = typeof kind === "string" ? `${event.topic}.${kind}` : event.topic;
        for (const [key, value] of Object.entries(event.values)) {
            if (typeof value !== "number" || !key.endsWith("_ms")) {
                continue;
            }
            const name = `${prefix}.${key}`;
            const list = series.get(name) ?? [];
            list.push(value);
            series.set(name, list);
        }
    }
    return series;
}

export function summarize(events: PerfEvent[]): Record<string, Stats> {
    const summary: Record<string, Stats> = {};
    for (const [name, values] of aggregate(events)) {
        const stats = computeStats(values);
        if (stats !== null) {
            summary[name] = stats;
        }
    }
    return summary;
}

interface TraceEvent {
    name: string;
    cat: string;
    ph: "X";
    ts: number;
    dur: number;
    pid: number;
    tid: number;
    args: Record<string, string | number>;
}

/// Convert perf events to Chrome "Trace Event" JSON (load in Perfetto or
/// chrome://tracing). A perf line is emitted when its span ends and carries
/// the duration, so the span is reconstructed as [ts - dur, ts]. Lines
/// without a timestamp are skipped.
export function toChromeTrace(events: PerfEvent[]): string {
    const traceEvents: TraceEvent[] = [];
    const base = events.find((e) => e.ts !== null)?.ts ?? 0;
    for (const event of events) {
        if (event.ts === null) {
            continue;
        }
        const duration = event.values["total_ms"] ?? event.values["elapsed_ms"];
        if (typeof duration !== "number") {
            continue;
        }
        const kind = event.values["kind"] ?? event.values["phase"];
        const name = typeof kind === "string" ? `${event.topic}.${kind}` : event.topic;
        traceEvents.push({
            name,
            cat: event.topic,
            ph: "X",
            ts: (event.ts - base - duration) * 1000,
            dur: duration * 1000,
            pid: 1,
            tid: 1,
            args: event.values,
        });
    }
    return JSON.stringify({ traceEvents, displayTimeUnit: "ms" });
}
