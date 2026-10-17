/// The nightly run of real projects (tests/nightly/projects.json), one job
/// at a time; tests/README.md states what fails it.
///
/// Usage:
///   node tools/nightly/run.ts --jobs
///   node tools/nightly/run.ts <job> --dir <dir> [options]
///
/// Options:
///   --dir <dir>        where projects are checked out and prepared; a
///                      checkout already prepared there is reused
///   --steps <list>     of index, sweep (default both); the project is always
///                      prepared first
///   --file <path>      sweep this checkout-relative file instead of a sample
///                      (repeatable)
///   --only <list>      sweep only these requests
///   --seed <text>      what picks the sampled files (default: today's date)
///   --out <dir>        write results.json and report.md there
///   --previous <file>  the previous night's results.json, for the comparison
///
/// --jobs prints every job, the name of its artifacts and the system
/// packages it needs, as JSON.
/// CLICE_EXECUTABLE names the clice under test.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { cliceExecutable } from "../client/session.ts";
import { runNightly, type Results, type Step } from "./nightly.ts";
import { findJob, jobs, loadProjects } from "./projects.ts";
import { report } from "./report.ts";
import { NAVIGATION, REQUESTS, type Request } from "./sweep.ts";

const { values, positionals } = parseArgs({
    options: {
        jobs: { type: "boolean", default: false },
        dir: { type: "string" },
        steps: { type: "string", default: "index,sweep" },
        file: { type: "string", multiple: true },
        only: { type: "string" },
        seed: { type: "string", default: new Date().toISOString().slice(0, 10) },
        out: { type: "string" },
        previous: { type: "string" },
    },
    allowPositionals: true,
});

const projects = loadProjects();
if (values.jobs) {
    console.log(
        JSON.stringify(
            jobs(projects).map((job) => ({
                name: job.name,
                artifact: job.name.replaceAll("/", "-"),
                packages: (job.project.packages ?? []).join(" "),
            })),
        ),
    );
    process.exit(0);
}

const [name] = positionals;
if (name === undefined || values.dir === undefined) {
    console.error("usage: node tools/nightly/run.ts <job> --dir <dir> [options]");
    process.exit(2);
}

function list<T extends string>(
    text: string | undefined,
    known: readonly T[],
    what: string,
): Set<T> | undefined {
    if (text === undefined) {
        return undefined;
    }
    const items = text.split(",");
    const unknown = items.filter((item) => !(known as readonly string[]).includes(item));
    if (unknown.length > 0) {
        console.error(`unknown ${what}: ${unknown.join(", ")}; known: ${known.join(", ")}`);
        process.exit(2);
    }
    return new Set(items as T[]);
}

const steps = list<Step>(values.steps, ["index", "sweep"], "steps") ?? new Set<Step>();
const only = list<Request>(values.only, [...REQUESTS, ...NAVIGATION, "diagnostics"], "requests");
const dir = path.resolve(values.dir);
fs.mkdirSync(dir, { recursive: true });

const results = await runNightly({
    job: findJob(projects, name),
    dir,
    clice: cliceExecutable(),
    steps,
    seed: values.seed,
    ...(values.file === undefined ? {} : { files: values.file }),
    ...(only === undefined ? {} : { only }),
});
const previous =
    values.previous !== undefined && fs.existsSync(values.previous)
        ? (JSON.parse(fs.readFileSync(values.previous, "utf8")) as Results)
        : undefined;
const text = report(results, previous);
if (values.out === undefined) {
    console.log(text);
} else {
    fs.mkdirSync(values.out, { recursive: true });
    fs.writeFileSync(
        path.join(values.out, "results.json"),
        `${JSON.stringify(results, null, 2)}\n`,
    );
    fs.writeFileSync(path.join(values.out, "report.md"), text);
    console.log(text);
}
process.exit(results.failures.length === 0 ? 0 : 1);
