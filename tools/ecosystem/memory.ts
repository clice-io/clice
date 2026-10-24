/// Memory and CPU of a process tree, measured from outside it and sampled
/// every second: the root's high-water mark, the largest of its
/// descendants', the most the tree held at once, the most each role held
/// (the master, each worker by its `--worker-name`), and the CPU seconds
/// the tree spent. What a process holds is its own memory: anonymous and
/// shared pages on Linux, the physical footprint `top` reports on macOS,
/// the private commit on Windows; its high-water mark is the system's on
/// Windows, the largest sample elsewhere (Linux's VmHWM counts the pages
/// mapped from files too, clice's PCHs and LMDB index among them). A
/// descendant that ends between two samples keeps the peak and the CPU seen
/// last. The tree's total counts each process's own memory only: the pages
/// of a binary its processes share are not counted once per process.

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

/// One process at one sample: memory in megabytes, CPU in seconds.
interface Usage {
    pid: number;
    parent: number;
    own: number;
    peak: number;
    cpu: number;
    /// When it started, where its parent's id may name a later process.
    started?: number;
    /// Its command line, where the sample read it.
    args?: string;
}

/// The kernel's clock ticks a second, in which /proc counts CPU time.
const TICKS = 100;

function procField(status: string, name: string): number {
    return Math.round(Number(new RegExp(`^${name}:\\s+(\\d+)`, "m").exec(status)?.[1] ?? 0) / 1024);
}

/// Every process from /proc; the command line is read for the tree only.
function procUsage(): Usage[] {
    return fs.readdirSync("/proc").flatMap((pid) => {
        if (!/^\d+$/.test(pid)) {
            return [];
        }
        try {
            const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
            const status = fs.readFileSync(`/proc/${pid}/status`, "utf8");
            // The command name in parentheses may hold spaces: the fields
            // after it are what counts, the parent the second, the user and
            // system times the twelfth and thirteenth.
            const fields = stat.slice(stat.lastIndexOf(")") + 2).split(" ");
            const own = procField(status, "RssAnon") + procField(status, "RssShmem");
            return [
                {
                    pid: Number(pid),
                    parent: Number(fields[1]),
                    own,
                    peak: own,
                    cpu: (Number(fields[11]) + Number(fields[12])) / TICKS,
                    started: Number(fields[19]),
                },
            ];
        } catch {
            return [];
        }
    });
}

function procArgs(pid: number): string {
    try {
        return fs
            .readFileSync(`/proc/${String(pid)}/cmdline`, "utf8")
            .replaceAll("\0", " ")
            .trim();
    } catch {
        return "";
    }
}

/// Where /proc is missing, a script printing a sample of every process,
/// tab-separated lines, and the same every second, a sample ending with an
/// empty line. macOS's footprint comes from top (`M pid kilobytes`), the
/// rest from ps (`P pid parent time args`); Windows names it all at once
/// (`W pid parent kilobytes peak started cpu args`).
const LISTING: Partial<
    Record<NodeJS.Platform, { shell: string[]; once: string; loop: (once: string) => string }>
> = {
    darwin: {
        shell: ["sh", "-c"],
        // top prints the footprint with a unit and a trailing +/- for the
        // change since the last sample.
        once:
            "top -l 1 -stats pid,mem | awk '$1 ~ /^[0-9]+$/ " +
            '{ v = $2; sub(/[+-]$/, "", v); u = substr(v, length(v)); n = substr(v, 1, length(v) - 1); ' +
            'm = u == "G" ? 1048576 : u == "M" ? 1024 : u == "K" ? 1 : 1 / 1024; ' +
            'printf "M\\t%d\\t%d\\n", $1, n * m }\'; ' +
            'ps -axo pid=,ppid=,time=,args= | awk \'{ p = $1; q = $2; t = $3; $1 = $2 = $3 = ""; ' +
            'sub(/^ +/, ""); printf "P\\t%s\\t%s\\t%s\\t%s\\n", p, q, t, $0 }\'',
        loop: (once) => `while :; do ${once}; echo; sleep 1; done`,
    },
    win32: {
        shell: ["powershell", "-NoProfile", "-Command"],
        once: 'Get-CimInstance Win32_Process | ForEach-Object { "W`t$($_.ProcessId)`t$($_.ParentProcessId)`t$($_.PageFileUsage)`t$($_.PeakPageFileUsage)`t$($_.CreationDate.Ticks)`t$($_.KernelModeTime + $_.UserModeTime)`t$($_.CommandLine)" }',
        loop: (once) => `while ($true) { ${once}; ''; Start-Sleep -Seconds 1 }`,
    },
};

/// ps's CPU time, `[[dd-]hh:]mm:ss.cc`, in seconds.
function psTime(text: string): number {
    const [days, clock] = text.includes("-") ? text.split("-") : ["0", text];
    return (
        Number(days) * 86_400 +
        (clock ?? "").split(":").reduce((seconds, part) => seconds * 60 + Number(part), 0)
    );
}

export function parsed(lines: readonly string[]): Usage[] {
    const footprint = new Map<number, number>();
    const usages: Usage[] = [];
    for (const line of lines) {
        const [kind, ...fields] = line.replace(/\r$/, "").split("\t");
        if (kind === "M") {
            footprint.set(Number(fields[0]), Math.round(Number(fields[1]) / 1024));
        } else if (kind === "P") {
            const [pid, parent, time, ...args] = fields;
            usages.push({
                pid: Number(pid),
                parent: Number(parent),
                own: 0,
                peak: 0,
                cpu: psTime(time ?? "0"),
                args: args.join("\t"),
            });
        } else if (kind === "W") {
            const [pid, parent, own, peak, started, cpu, ...args] = fields;
            usages.push({
                pid: Number(pid),
                parent: Number(parent),
                own: Math.round(Number(own) / 1024),
                peak: Math.round(Number(peak) / 1024),
                cpu: Number(cpu) / 1e7,
                ...(started === undefined || started === "" ? {} : { started: Number(started) }),
                args: args.join("\t"),
            });
        }
    }
    for (const usage of usages) {
        const own = footprint.get(usage.pid);
        if (own !== undefined) {
            usage.own = own;
            usage.peak = own;
        }
    }
    return usages.filter((usage) => Number.isInteger(usage.pid) && Number.isInteger(usage.parent));
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
    return parsed(
        execFileSync(command, [...args, once], { encoding: "utf8", maxBuffer: 1 << 26 }).split(
            /\r?\n/,
        ),
    );
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

function resident(usages: readonly Usage[], root: number): Resident {
    const now: Resident = { root: 0, children: 0 };
    for (const usage of tree(usages, root)) {
        if (usage.pid === root) {
            now.root = usage.own;
        } else {
            now.children += usage.own;
        }
    }
    return now;
}

export function residentNow(root: number): Resident {
    return resident(usageNow(), root);
}

/// The tree at one sample: milliseconds since the watch began, megabytes
/// all of it and each role held, the CPU seconds it had spent.
export interface Sample {
    t: number;
    total: number;
    roles: Record<string, number>;
    cpu: number;
}

export interface TreeMemory {
    peak: PeakMemory;
    /// What the tree held at the last sample.
    end: Resident;
    /// The most each role held at once.
    roles: Record<string, number>;
    cpu: number;
}

export interface TreeWatch {
    /// The newest sample, none before the first.
    latest(): Sample | undefined;
    timeline(): readonly Sample[];
    /// Takes one last sample, so a tree watched for less than a second still
    /// has its peak, and ends the watch.
    stop(): TreeMemory & { timeline: Sample[] };
}

/// Samples the tree under `root` every second. A process's role is its
/// `--worker-name`, `rootRole` for the root and for a server the root
/// wraps, and `other` for the rest.
export function watchTree(root: number, rootRole = "master"): TreeWatch {
    const started = Date.now();
    const peak: PeakMemory = { root: 0, child: 0, total: 0 };
    const roles: Record<string, number> = {};
    const timeline: Sample[] = [];
    const argsOf = new Map<number, string>();
    const lastCpu = new Map<number, number>();
    let gone = 0;
    const roleOf = (usage: Usage): string => {
        if (usage.pid === root) {
            return rootRole;
        }
        let args = usage.args ?? argsOf.get(usage.pid);
        if (args === undefined) {
            args = procArgs(usage.pid);
            argsOf.set(usage.pid, args);
        }
        // A server behind a wrapper (a memory limit's) is the root's role.
        const server = /(^|[\\/])(clice|clangd)(\.exe)?$/.test(args.split(" ")[0] ?? "");
        return /--worker-name[ =](\S+)/.exec(args)?.[1] ?? (server ? rootRole : "other");
    };
    const look = (usages: readonly Usage[]) => {
        const sample: Sample = { t: Date.now() - started, total: 0, roles: {}, cpu: 0 };
        const seen = new Set<number>();
        for (const usage of tree(usages, root)) {
            seen.add(usage.pid);
            sample.total += usage.own;
            const role = roleOf(usage);
            sample.roles[role] = (sample.roles[role] ?? 0) + usage.own;
            const high = Math.max(usage.own, usage.peak);
            if (usage.pid === root) {
                peak.root = Math.max(peak.root, high);
            } else {
                peak.child = Math.max(peak.child, high);
            }
            lastCpu.set(usage.pid, usage.cpu);
        }
        for (const [pid, cpu] of lastCpu) {
            if (!seen.has(pid)) {
                gone += cpu;
                lastCpu.delete(pid);
            }
        }
        sample.cpu =
            Math.round((gone + [...lastCpu.values()].reduce((a, b) => a + b, 0)) * 10) / 10;
        peak.total = Math.max(peak.total, sample.total);
        for (const [role, megabytes] of Object.entries(sample.roles)) {
            roles[role] = Math.max(roles[role] ?? 0, megabytes);
        }
        timeline.push(sample);
    };
    const last = () => {
        const usages = usageNow();
        look(usages);
        return {
            peak,
            end: resident(usages, root),
            roles,
            cpu: timeline.at(-1)?.cpu ?? 0,
            timeline,
        };
    };
    const latest = () => timeline.at(-1);
    if (process.platform === "linux") {
        // Time is the feature: memory announces nothing, a sample a second is
        // the measurement.
        const timer = setInterval(() => {
            look(procUsage());
        }, 1000);
        return {
            latest,
            timeline: () => timeline,
            stop: () => {
                clearInterval(timer);
                return last();
            },
        };
    }
    const { shell, once, loop } = listing();
    const [command = "", ...args] = shell;
    const sampler = spawn(command, [...args, loop(once)], { stdio: ["ignore", "pipe", "ignore"] });
    // The last sample stop() takes is then the only one.
    sampler.on("error", (error) => {
        console.warn(`cannot sample memory: ${error.message}`);
    });
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
        latest,
        timeline: () => timeline,
        stop: () => {
            sampler.kill();
            return last();
        },
    };
}
