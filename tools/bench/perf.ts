/// Parser and aggregator for clice's `[perf:<topic>] key=value` log lines
/// (see src/support/logging.h). Consumed by bench.ts for the server-side
/// breakdown of a benchmark run and by perf_report.ts for offline log
/// analysis.

/// One `[perf:topic]` line: string keys kept verbatim, numeric values
/// parsed. `ts` is the log line's timestamp in epoch milliseconds when the
/// line carried one (stderr mirrors and log files do). `pid`/`tid` are the
/// trace lanes: the caller assigns one `pid` per source log (master and
/// each worker are separate processes), `tid` is the line's `[thread N]`
/// value; both default to 1 for stderr-only sources.
export interface PerfEvent {
    topic: string;
    ts: number | null;
    pid: number;
    tid: number;
    values: Record<string, string | number>;
}

/// `[2026-08-14 12:34:56.789] [info] [thread 1] [file.cpp:42] [perf:x] k=v`
const LINE_PATTERN =
    /^(?:\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\] )?.*?\[perf:([\w-]+)\] (.*)$/;

const THREAD_PATTERN = /\[thread (\d+)\]/;

export function parsePerfLines(text: string, pid = 1): PerfEvent[] {
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
        // A value runs until the next ` key=` boundary, not the next space:
        // paths may contain spaces. This boundary is the format's contract —
        // producers must not put arbitrary user text into perf lines (the
        // workspace-symbol query is logged as query_len for this reason);
        // the remaining values are filesystem paths and enum names.
        for (const pair of rest.matchAll(/(\w+)=((?:(?!\s\w+=).)*)/g)) {
            const [, key, raw] = pair;
            if (key === undefined || raw === undefined) {
                continue;
            }
            const num = Number(raw);
            values[key] = raw !== "" && Number.isFinite(num) ? num : raw;
        }
        const thread = THREAD_PATTERN.exec(line);
        events.push({
            topic,
            // The log stamp has no timezone; Date.parse reads it as local
            // time, which is what the producing process used.
            ts: stamp !== undefined ? Date.parse(stamp.replace(" ", "T")) : null,
            pid,
            tid: thread?.[1] !== undefined ? Number(thread[1]) : 1,
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

/// Discriminated name for an event: `<topic>[.<kind>][.<detail>]`. The kind
/// is the event's `kind`, `phase`, or `op` value; the detail is its `scope`
/// or `rel` value — mirroring how the topics in logging.h use those fields
/// to distinguish materially different operations (`index_detail op=build
/// scope=full` vs `scope=interested`, `index_query kind=relations
/// rel=Definition` vs `rel=Reference`).
function eventName(event: PerfEvent): string {
    const parts = [event.topic];
    const kind = event.values["kind"] ?? event.values["phase"] ?? event.values["op"];
    if (kind !== undefined) {
        parts.push(String(kind));
    }
    const detail = event.values["scope"] ?? event.values["rel"];
    if (detail !== undefined) {
        parts.push(String(detail));
    }
    return parts.join(".");
}

/// Group events into duration series. Every numeric `*_ms` key becomes a
/// series named `<eventName>.<key>`.
function aggregate(events: PerfEvent[]): Map<string, number[]> {
    const series = new Map<string, number[]>();
    for (const event of events) {
        const prefix = eventName(event);
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

interface TraceMetadataEvent {
    name: "process_name";
    ph: "M";
    pid: number;
    args: { name: string };
}

/// Convert perf events to Chrome "Trace Event" JSON (load in Perfetto or
/// chrome://tracing). A perf line is emitted when its span ends and carries
/// the duration, so the span is reconstructed as [ts - dur, ts]. Lines
/// without a timestamp are skipped. `processNames` labels the pid lanes
/// (typically the source log file names).
export function toChromeTrace(
    events: PerfEvent[],
    processNames: Record<number, string> = {},
): string {
    const traceEvents: (TraceEvent | TraceMetadataEvent)[] = [];
    for (const [pid, name] of Object.entries(processNames)) {
        traceEvents.push({ name: "process_name", ph: "M", pid: Number(pid), args: { name } });
    }
    // Events concatenate from multiple log files in file order, so the
    // first stamped event is not necessarily the earliest. No spread into
    // Math.min: a long session's event count exceeds the engine's
    // argument-count limit.
    let base = Infinity;
    for (const event of events) {
        if (event.ts !== null && event.ts < base) {
            base = event.ts;
        }
    }
    for (const event of events) {
        if (event.ts === null) {
            continue;
        }
        const duration = event.values["total_ms"] ?? event.values["elapsed_ms"];
        if (typeof duration !== "number") {
            continue;
        }
        traceEvents.push({
            name: eventName(event),
            cat: event.topic,
            ph: "X",
            ts: (event.ts - base - duration) * 1000,
            dur: duration * 1000,
            pid: event.pid,
            tid: event.tid,
            args: event.values,
        });
    }
    return JSON.stringify({ traceEvents, displayTimeUnit: "ms" });
}
