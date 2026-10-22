/// Peak memory of a process tree, measured from outside it: the root's
/// high-water mark, the largest of its descendants', and the most the tree
/// held at once, sampled every second. What a process holds is its own
/// memory where the system tells it apart (anonymous and shared pages on
/// Linux, the private commit on Windows) and its resident size on macOS,
/// whose `ps` tells no more; its high-water mark is the system's on Linux
/// and Windows, the largest sample on macOS. A descendant that ends between
/// two samples keeps the peak seen last. The tree's total counts each
/// process's own memory only: the pages of a binary a thousand processes
/// share are there once on Linux and Windows.

import { execFileSync, spawn } from "node:child_process";
import * as fs from "node:fs";

export interface PeakMemory {
    /// Megabytes.
    root: number;
    child: number;
    total: number;
}

/// Megabytes a process tree holds at one moment, each process's own.
export interface Resident {
    root: number;
    children: number;
}

/// One process at one sample, in megabytes.
interface Usage {
    pid: number;
    parent: number;
    own: number;
    peak: number;
    /// When it started, where its parent's id may name a later process.
    started?: number;
}

/// Every process from /proc.
function procUsage(): Usage[] {
    return fs.readdirSync("/proc").flatMap((pid) => {
        if (!/^\d+$/.test(pid)) {
            return [];
        }
        try {
            const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
            const status = fs.readFileSync(`/proc/${pid}/status`, "utf8");
            const field = (name: string): number =>
                Math.round(
                    Number(new RegExp(`^${name}:\\s+(\\d+)`, "m").exec(status)?.[1] ?? 0) / 1024,
                );
            return [
                {
                    pid: Number(pid),
                    // The command name in parentheses may hold spaces: the
                    // fields after it are what counts, the parent the second.
                    parent: Number(stat.slice(stat.lastIndexOf(")") + 2).split(" ")[1]),
                    own: field("RssAnon") + field("RssShmem"),
                    peak: field("VmHWM"),
                },
            ];
        } catch {
            return [];
        }
    });
}

/// Where /proc is missing, a script printing a line `pid parent own [peak
/// started]` for every process, its memory in kilobytes, and the same every
/// second, a sample ending with an empty line.
const LISTING: Partial<
    Record<NodeJS.Platform, { shell: string[]; once: string; loop: (once: string) => string }>
> = {
    darwin: {
        shell: ["sh", "-c"],
        once: "ps -A -o pid=,ppid=,rss=",
        loop: (once) => `while :; do ${once}; echo; sleep 1; done`,
    },
    win32: {
        shell: ["powershell", "-NoProfile", "-Command"],
        once: 'Get-CimInstance Win32_Process | ForEach-Object { "$($_.ProcessId) $($_.ParentProcessId) $($_.PageFileUsage) $($_.PeakPageFileUsage) $($_.CreationDate.Ticks)" }',
        loop: (once) => `while ($true) { ${once}; ''; Start-Sleep -Seconds 1 }`,
    },
};

function parsed(lines: readonly string[]): Usage[] {
    return lines.flatMap((line) => {
        const [pid, parent, own, peak, started] = line.trim().split(/\s+/).map(Number);
        return pid === undefined || parent === undefined || own === undefined
            ? []
            : [
                  {
                      pid,
                      parent,
                      own: Math.round(own / 1024),
                      peak: Math.round((peak ?? own) / 1024),
                      ...(started === undefined ? {} : { started }),
                  },
              ];
    });
}

function listing(): NonNullable<(typeof LISTING)[NodeJS.Platform]> {
    const found = LISTING[process.platform];
    if (found === undefined) {
        throw new Error(`no way to read process memory on ${process.platform}`);
    }
    return found;
}

function usageNow(): Usage[] {
    if (process.platform === "linux") {
        return procUsage();
    }
    const { shell, once } = listing();
    const [command = "", ...args] = shell;
    return parsed(execFileSync(command, [...args, once], { encoding: "utf8" }).split(/\r?\n/));
}

/// The processes of the tree under `root` in a sample.
function tree(usages: readonly Usage[], root: number): Usage[] {
    const children = new Map<number, Usage[]>();
    for (const usage of usages) {
        const siblings = children.get(usage.parent);
        if (siblings === undefined) {
            children.set(usage.parent, [usage]);
        } else {
            siblings.push(usage);
        }
    }
    const out = usages.filter((usage) => usage.pid === root);
    // The iteration reaches the children pushed on the way.
    for (const parent of out) {
        // Windows keeps a process's parent id after the parent exits, and
        // reuses it: a process started before one naming it is not its child.
        const born = (child: Usage) =>
            parent.started === undefined ||
            child.started === undefined ||
            child.started >= parent.started;
        out.push(...(children.get(parent.pid) ?? []).filter(born));
    }
    return out;
}

export function watchMemory(root: number): { stop: () => PeakMemory } {
    const peak: PeakMemory = { root: 0, child: 0, total: 0 };
    const look = (usages: readonly Usage[]) => {
        let total = 0;
        for (const usage of tree(usages, root)) {
            total += usage.own;
            const high = Math.max(usage.own, usage.peak);
            if (usage.pid === root) {
                peak.root = Math.max(peak.root, high);
            } else {
                peak.child = Math.max(peak.child, high);
            }
        }
        peak.total = Math.max(peak.total, total);
    };
    if (process.platform === "linux") {
        // Time is the feature: memory announces nothing, a sample a second is
        // the measurement.
        const timer = setInterval(() => {
            look(procUsage());
        }, 1000);
        return {
            stop: () => {
                clearInterval(timer);
                return peak;
            },
        };
    }
    const { shell, once, loop } = listing();
    const [command = "", ...args] = shell;
    const sampler = spawn(command, [...args, loop(once)], { stdio: ["ignore", "pipe", "ignore"] });
    let pending = "";
    let sample: string[] = [];
    sampler.stdout.setEncoding("utf8").on("data", (chunk: string) => {
        const lines = (pending + chunk).split(/\r?\n/);
        pending = lines.pop() ?? "";
        for (const line of lines) {
            if (line.trim() === "") {
                look(parsed(sample));
                sample = [];
            } else {
                sample.push(line);
            }
        }
    });
    return {
        stop: () => {
            sampler.kill();
            return peak;
        },
    };
}

export function residentNow(root: number): Resident {
    const now: Resident = { root: 0, children: 0 };
    for (const usage of tree(usageNow(), root)) {
        if (usage.pid === root) {
            now.root = usage.own;
        } else {
            now.children += usage.own;
        }
    }
    return now;
}
