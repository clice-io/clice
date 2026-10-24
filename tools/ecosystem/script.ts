/// A project's test.ts (ecosystem/README.md): the jobs it registers, the
/// places its perf scenarios ask at, and the failures it knows.

import * as fs from "node:fs";
import * as path from "node:path";
import { pathToFileURL } from "node:url";
import {
    resolveLocation,
    SourceText,
    withoutBom,
    type Position,
} from "../testing/render/position.ts";
import type { Perf } from "./perf/scenarios.ts";
import {
    configurationOf,
    configurationPart,
    ECOSYSTEM,
    loadProjects,
    platformsOf,
    type Configuration,
    type Platform,
    type Project,
} from "./project.ts";

/// A failure a project knows: its board issue, and what it matches.
export interface Known {
    issue: string;
    /// What goes wrong, for a reader of the report.
    why: string;
    configuration?: string;
    part?: string;
    platform?: Platform;
    kind?: string;
    request?: string;
    /// A checkout-relative path, or a pattern over one.
    file?: string | RegExp;
}

export interface RegressionOptions {
    /// Default: every configuration of the project.
    configurations?: string[];
    /// The source files a night sweeps, and a quarter as many headers.
    sample?: number;
    known?: Known[];
}

export interface PerfOptions {
    /// Default: the project's only configuration.
    configuration?: string;
    /// Default: every platform of the configuration.
    platforms?: Platform[];
    /// Arguments clangd takes beside its defaults, for what it does only on
    /// request (C++20 modules).
    clangdArgs?: string[];
    known?: Known[];
}

export type PerfScript = (p: Perf) => Promise<void>;

interface Registered {
    regressions: RegressionOptions[];
    perfs: { name: string; options: PerfOptions; script: PerfScript }[];
}

let registering: Registered | undefined;

function current(what: string): Registered {
    if (registering === undefined) {
        throw new Error(`${what}() registers a job: call it at the top level of a test.ts`);
    }
    return registering;
}

export function regression(options: RegressionOptions = {}): void {
    current("regression").regressions.push(options);
}

export function perf(name: string, options: PerfOptions, script: PerfScript): void {
    const registered = current("perf");
    if (registered.perfs.some((each) => each.name === name)) {
        throw new Error(`two perf jobs are named ${name}`);
    }
    registered.perfs.push({ name, options, script });
}

/// What each script registered, by file: the default script is imported
/// once and serves every project without a test.ts.
const scripts = new Map<string, Registered>();

async function scriptOf(project: Project): Promise<Registered> {
    const own = path.join(ECOSYSTEM, project.name, "test.ts");
    const file = fs.existsSync(own) ? own : path.join(ECOSYSTEM, "test.ts");
    const known = scripts.get(file);
    if (known !== undefined) {
        return known;
    }
    registering = { regressions: [], perfs: [] };
    try {
        await import(pathToFileURL(file).href);
        scripts.set(file, registering);
        return registering;
    } finally {
        registering = undefined;
    }
}

interface JobBase {
    name: string;
    project: Project;
    configuration: string;
    config: Configuration;
    platforms: readonly Platform[];
    known: readonly Known[];
}

export interface RegressionJob extends JobBase {
    kind: "regression";
    part?: string;
    sample: number;
}

export interface PerfJob extends JobBase {
    kind: "perf";
    clangdArgs: readonly string[];
    script: PerfScript;
}

export type Job = RegressionJob | PerfJob;
export type JobKind = Job["kind"];

const SAMPLE = 40;

function onlyConfiguration(project: Project): string {
    const names = Object.keys(project.configurations);
    const [only] = names;
    if (names.length !== 1 || only === undefined) {
        throw new Error(
            `${project.name} has the configurations ${names.join(", ")}: a perf job names one`,
        );
    }
    return only;
}

export async function jobsOf(project: Project): Promise<Job[]> {
    const { regressions, perfs } = await scriptOf(project);
    const jobs: Job[] = [];
    for (const options of regressions) {
        for (const configuration of options.configurations ?? Object.keys(project.configurations)) {
            const config = configurationOf(project, configuration);
            const base = {
                kind: "regression" as const,
                project,
                configuration,
                config,
                platforms: platformsOf(config),
                known: options.known ?? [],
                sample: options.sample ?? SAMPLE,
            };
            const named = [project.name, ...configurationPart(project, configuration)];
            if (config.parts === undefined) {
                jobs.push({ ...base, name: named.join("/") });
            } else {
                for (const part of Object.keys(config.parts)) {
                    jobs.push({ ...base, name: [...named, part].join("/"), part });
                }
            }
        }
    }
    for (const { name, options, script } of perfs) {
        const configuration = options.configuration ?? onlyConfiguration(project);
        const config = configurationOf(project, configuration);
        const outside = (options.platforms ?? []).filter(
            (platform) => !platformsOf(config).includes(platform),
        );
        if (outside.length > 0) {
            throw new Error(
                `perf ${project.name}/${name}: ${configuration} does not build on ${outside.join(", ")}`,
            );
        }
        jobs.push({
            kind: "perf",
            name: `${project.name}/${name}`,
            project,
            configuration,
            config,
            platforms: options.platforms ?? platformsOf(config),
            known: options.known ?? [],
            clangdArgs: options.clangdArgs ?? [],
            script,
        });
    }
    const names = jobs.map((job) => `${job.kind} ${job.name}`);
    const twice = names.find((name, i) => names.indexOf(name) !== i);
    if (twice !== undefined) {
        throw new Error(`${project.name} registers ${twice} twice`);
    }
    return jobs;
}

export async function allJobs(): Promise<Job[]> {
    const jobs: Job[] = [];
    for (const project of loadProjects()) {
        jobs.push(...(await jobsOf(project)));
    }
    return jobs;
}

export async function findJob(kind: JobKind, name: string): Promise<Job> {
    const all = (await allJobs()).filter((job) => job.kind === kind);
    const job = all.find((each) => each.name === name);
    if (job === undefined) {
        throw new Error(
            `no ${kind} job ${name}; the ${kind} jobs are ${all.map((each) => each.name).join(", ")}`,
        );
    }
    return job;
}

/// What failed in a job, and how to see it again.
export interface Failure {
    kind: string;
    file?: string;
    /// The request, and the place it was asked at.
    request?: string;
    at?: string;
    detail: string;
    /// The command that reproduces it, run from the repository root.
    repro: string;
}

/// The known issue a failure is, if any.
export function knownIssue(job: Job, platform: Platform, failure: Failure): Known | undefined {
    const part = job.kind === "regression" ? job.part : undefined;
    return job.known.find(
        (known) =>
            (known.configuration === undefined || known.configuration === job.configuration) &&
            (known.part === undefined || known.part === part) &&
            (known.platform === undefined || known.platform === platform) &&
            (known.kind === undefined || known.kind === failure.kind) &&
            (known.request === undefined || known.request === failure.request) &&
            (known.file === undefined ||
                (failure.file !== undefined &&
                    (typeof known.file === "string"
                        ? known.file === failure.file
                        : known.file.test(failure.file)))),
    );
}

/// The failures of a job the project knows moved apart, each with its
/// issue, and the known issues the job did not see.
export function sortKnown(
    job: Job,
    platform: Platform,
    results: { failures: Failure[]; known: (Failure & { issue: string })[]; unseen: Known[] },
): void {
    const fresh: Failure[] = [];
    const seen = new Set<Known>();
    for (const failure of results.failures) {
        const known = knownIssue(job, platform, failure);
        if (known === undefined) {
            fresh.push(failure);
        } else {
            seen.add(known);
            results.known.push({ ...failure, issue: known.issue });
        }
    }
    results.failures = fresh;
    results.unseen = job.known.filter((known) => !seen.has(known));
}

/// A line of a file named by its code, the place in `⟦` `⟧`; a line the
/// file repeats is the first such after the one `after` names.
export type Place = string | { after: string; at: string };

export const PROBES = ["symbol", "member", "unqualified", "template", "call"] as const;
export type Probe = (typeof PROBES)[number];

/// A file perf scenarios open, and where they ask: `symbol` for hover,
/// definition, references and the call hierarchy, `member`, `unqualified`
/// and `template` for completion, `call` for signature help; `query` the
/// workspace symbol query, by default the identifier at `symbol`.
export type PerfFile = { file: string; query?: string } & Partial<Record<Probe, Place>>;

function sourceOf(root: string, file: string): SourceText {
    return new SourceText(file, withoutBom(fs.readFileSync(path.join(root, file), "utf8")));
}

/// Where a place is in a file of the checkout `root`.
export function positionOf(root: string, file: string, place: Place): Position {
    const source = sourceOf(root, file);
    const scope = { file: () => source, query: { path: file } };
    if (typeof place === "string") {
        return resolveLocation(place, scope).range.start;
    }
    const after = resolveLocation(`⟦${place.after}⟧`, scope).range.start.line;
    const plain = place.at.replaceAll("⟦", "").replaceAll("⟧", "").trim();
    const line = source.lines.findIndex((text, index) => index > after && text.trim() === plain);
    if (line === -1) {
        throw new Error(`${file} has no '${place.at}' after '${place.after}'`);
    }
    return resolveLocation(`${place.at} @${String(line + 1)}`, scope).range.start;
}

/// The identifier a position is on.
export function identifierAt(root: string, file: string, at: Position): string {
    const line = sourceOf(root, file).line(at.line);
    const start = /\w*$/.exec(line.slice(0, at.character))?.index ?? at.character;
    const name = /^[A-Za-z_]\w*/.exec(line.slice(start))?.[0];
    if (name === undefined) {
        throw new Error(`${file}: no identifier at its symbol position; give a query`);
    }
    return name;
}
