/// Peak memory of a process tree on Linux, read from /proc: the root's
/// high-water mark, the largest of its descendants', and the most the tree
/// held at once. A descendant that ends between two looks keeps the peak
/// seen last.

import * as fs from "node:fs";

export interface PeakMemory {
    /// Megabytes.
    root: number;
    child: number;
    total: number;
}

function parentOf(pid: string): number | undefined {
    try {
        const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
        // The command name in parentheses may hold spaces: the fields after
        // it are what counts, the parent the second of them.
        return Number(stat.slice(stat.lastIndexOf(")") + 2).split(" ")[1]);
    } catch {
        return undefined;
    }
}

/// The resident and peak resident megabytes of a process.
function sizes(pid: number): { rss: number; peak: number } | undefined {
    try {
        const status = fs.readFileSync(`/proc/${String(pid)}/status`, "utf8");
        const field = (name: string): number =>
            Math.round(
                Number(new RegExp(`^${name}:\\s+(\\d+)`, "m").exec(status)?.[1] ?? 0) / 1024,
            );
        return { rss: field("VmRSS"), peak: field("VmHWM") };
    } catch {
        return undefined;
    }
}

function tree(root: number): number[] {
    const children = new Map<number, number[]>();
    for (const pid of fs.readdirSync("/proc").filter((name) => /^\d+$/.test(name))) {
        const parent = parentOf(pid);
        if (parent !== undefined) {
            children.set(parent, [...(children.get(parent) ?? []), Number(pid)]);
        }
    }
    const out: number[] = [];
    const stack = [root];
    for (let pid = stack.pop(); pid !== undefined; pid = stack.pop()) {
        out.push(pid);
        stack.push(...(children.get(pid) ?? []));
    }
    return out;
}

/// Look at the tree under `root` every second until stop() is called.
export function watchMemory(root: number): { stop: () => PeakMemory } {
    const peak: PeakMemory = { root: 0, child: 0, total: 0 };
    const look = () => {
        let total = 0;
        for (const pid of tree(root)) {
            const size = sizes(pid);
            if (size === undefined) {
                continue;
            }
            total += size.rss;
            if (pid === root) {
                peak.root = Math.max(peak.root, size.peak);
            } else {
                peak.child = Math.max(peak.child, size.peak);
            }
        }
        peak.total = Math.max(peak.total, total);
    };
    // Time is the feature: memory announces nothing, a sample a second is
    // the measurement.
    const timer = setInterval(look, 1000);
    return {
        stop: () => {
            clearInterval(timer);
            return peak;
        },
    };
}
