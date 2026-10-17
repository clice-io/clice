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
import * as os from "node:os";
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

// Memory is read from /proc, the kernel builds on Linux only, and the
// runners are Linux.
if (process.platform !== "linux") {
    console.error("the nightly jobs run on Linux");
    process.exit(2);
}

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

function list<T extends string>(text: string, known: readonly T[], what: string): Set<T> {
    const items = text.split(",");
    const unknown = items.filter((item) => !(known as readonly string[]).includes(item));
    if (unknown.length > 0) {
        console.error(`unknown ${what}: ${unknown.join(", ")}; known: ${known.join(", ")}`);
        process.exit(2);
    }
    return new Set(items as T[]);
}

const steps = list<Step>(values.steps, ["index", "sweep"], "steps");
const only =
    values.only === undefined
        ? undefined
        : list<Request>(values.only, [...REQUESTS, ...NAVIGATION, "errors"], "requests");
fs.mkdirSync(values.dir, { recursive: true });
// Build systems write the physical paths into their databases.
const dir = fs.realpathSync(values.dir);

/// `clice` behind a script that makes it, and every process it starts, the
/// first the kernel kills when memory runs out: a run that exhausts the
/// machine ends as clice's crash, which the job reports, instead of taking
/// this runner or the CI runner's agent with it.
function killedFirst(clice: string): string {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-nightly-"));
    // The answers' renderer finds clang's resource directory beside the
    // executable, as clice does.
    fs.symlinkSync(
        path.join(path.dirname(fs.realpathSync(clice)), "..", "lib"),
        path.join(root, "lib"),
    );
    fs.mkdirSync(path.join(root, "bin"));
    const wrapper = path.join(root, "bin", "clice");
    fs.writeFileSync(
        wrapper,
        `#!/bin/sh\necho 1000 > /proc/self/oom_score_adj\nexec '${clice.replaceAll("'", "'\\''")}' "$@"\n`,
        { mode: 0o755 },
    );
    return wrapper;
}

// A notification sent to a server that has died fails with nobody awaiting
// it; the server's exit check reports the death.
process.on("unhandledRejection", (reason) => {
    if ((reason as NodeJS.ErrnoException | undefined)?.code !== "ERR_STREAM_DESTROYED") {
        throw reason;
    }
});

const clice = cliceExecutable();
process.env["CLICE_EXECUTABLE"] = killedFirst(clice);

const results = await runNightly({
    job: findJob(projects, name),
    dir,
    clice,
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
