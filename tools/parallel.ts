/// Minimal worker pool shared by the TS test drivers (snap runner today;
/// any tool fanning out independent jobs — replay traces, corpus sweeps —
/// can reuse it). Node builtins only, so replay.ts-style standalone
/// scripts may depend on it.

import * as os from "node:os";

/// Run `worker` over every item with at most `width` concurrent executions,
/// preserving input order in the results. A worker that throws rejects the
/// whole run; workers that must survive per-item failures catch and encode
/// them in their result.
export async function mapParallel<T, R>(
    items: readonly T[],
    worker: (item: T) => Promise<R>,
    width: number = os.availableParallelism(),
): Promise<R[]> {
    const results = new Array<R>(items.length);
    let next = 0;
    const run = async () => {
        for (;;) {
            const i = next++;
            if (i >= items.length) {
                return;
            }
            // In-bounds by the check above; the index type alone cannot see it.
            results[i] = await worker(items[i] as T);
        }
    };
    const workers = Math.max(1, Math.min(width, items.length));
    await Promise.all(Array.from({ length: workers }, run));
    return results;
}
