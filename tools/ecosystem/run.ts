/// A round over the real projects of ecosystem/, one job at a time;
/// ecosystem/README.md states what each kind of job does and what fails it.
///
/// Usage:
///   node tools/ecosystem/run.ts --jobs
///   node tools/ecosystem/run.ts regression <job> --dir <dir> [options]
///   node tools/ecosystem/run.ts perf <job> --dir <dir> [options]
///
/// Options:
///   --dir <dir>          where projects are checked out and prepared; a
///                        checkout already prepared there is reused
///   --out <dir>          write results.json and report.md there
///   regression:
///   --steps <list>       of index, sweep (default both); the project is
///                        always prepared first
///   --file <path>        sweep this checkout-relative file instead of a
///                        sample (repeatable)
///   --only <list>        sweep only these requests
///   --seed <text>        what picks the sampled files (default: today's date)
///   --previous <file>    the previous round's results.json, for the comparison
///   perf:
///   --scenario <name>    run only these scenarios of the job (repeatable)
///   --clangd <path>      a clangd instead of the pinned release
///   --no-clangd          no clangd: clice against the baseline alone
///   --baseline <path>    a second clice to compare with
///   --drop-caches        drop the OS file cache before every cold run
///                        (Linux and macOS, through sudo)
///
/// --jobs prints every job of a round on every platform, the name of its
/// artifacts and the system packages it needs, as JSON.
/// CLICE_EXECUTABLE names the clice under test.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { cliceExecutable } from "../client/session.ts";
import { runPerf } from "./perf/perf.ts";
import { currentPlatform } from "./project.ts";
import { regressionReport, perfReport } from "./report.ts";
import { runRegression, type Results, type Step } from "./regression/regression.ts";
import { REQUESTS, type Request } from "./regression/sweep.ts";
import { allJobs, artifactName, findJob } from "./script.ts";

const { values, positionals } = parseArgs({
    options: {
        jobs: { type: "boolean", default: false },
        dir: { type: "string" },
        out: { type: "string" },
        steps: { type: "string", default: "index,sweep" },
        file: { type: "string", multiple: true },
        only: { type: "string" },
        seed: { type: "string", default: new Date().toISOString().slice(0, 10) },
        previous: { type: "string" },
        scenario: { type: "string", multiple: true },
        clangd: { type: "string" },
        "no-clangd": { type: "boolean", default: false },
        baseline: { type: "string" },
        "drop-caches": { type: "boolean", default: false },
    },
    allowPositionals: true,
});

if (values.jobs) {
    console.log(
        JSON.stringify(
            (await allJobs()).flatMap((job) =>
                job.platforms.map((platform) => ({
                    kind: job.kind,
                    name: job.name,
                    platform,
                    artifact: artifactName(job.kind, job.name, platform),
                    packages: platform === "linux" ? (job.config.packages ?? []).join(" ") : "",
                })),
            ),
        ),
    );
    process.exit(0);
}

const [kind, name] = positionals;
if ((kind !== "regression" && kind !== "perf") || name === undefined || values.dir === undefined) {
    console.error("usage: node tools/ecosystem/run.ts regression|perf <job> --dir <dir> [options]");
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

fs.mkdirSync(values.dir, { recursive: true });
// Build systems write the physical paths into their databases.
const dir = fs.realpathSync(values.dir);

/// `clice` behind a script that makes it, and every process it starts, the
/// first the kernel kills when memory runs out: a run that exhausts the
/// machine ends as clice's crash, which the job reports, instead of taking
/// this runner or the CI runner's agent with it.
function killedFirst(clice: string): string {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-ecosystem-"));
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
// it; the run's checks report the death.
process.on("unhandledRejection", (reason) => {
    if ((reason as NodeJS.ErrnoException | undefined)?.code !== "ERR_STREAM_DESTROYED") {
        throw reason;
    }
});

const platform = currentPlatform();
const job = await findJob(kind, name);
if (!job.platforms.includes(platform)) {
    console.error(`${kind} ${job.name} does not run on ${platform}`);
    process.exit(2);
}
const clice = cliceExecutable();

function write(results: unknown, text: string): void {
    console.log(text);
    if (values.out !== undefined) {
        fs.mkdirSync(values.out, { recursive: true });
        fs.writeFileSync(
            path.join(values.out, "results.json"),
            `${JSON.stringify(results, null, 1)}\n`,
        );
        fs.writeFileSync(path.join(values.out, "report.md"), text);
    }
}

if (job.kind === "regression") {
    // Only Linux lets a process choose to be killed first; a perf run times
    // the server as users run it.
    if (platform === "linux") {
        process.env["CLICE_EXECUTABLE"] = killedFirst(clice);
    }
    const only =
        values.only === undefined ? undefined : list<Request>(values.only, REQUESTS, "requests");
    const results = await runRegression({
        job,
        platform,
        dir,
        clice,
        steps: list<Step>(values.steps, ["index", "sweep"], "steps"),
        seed: values.seed,
        ...(values.file === undefined ? {} : { files: values.file }),
        ...(only === undefined ? {} : { only }),
    });
    const previous =
        values.previous !== undefined && fs.existsSync(values.previous)
            ? (JSON.parse(fs.readFileSync(values.previous, "utf8")) as Results)
            : undefined;
    write(results, regressionReport(results, previous));
    process.exit(results.failures.length === 0 ? 0 : 1);
} else {
    const results = await runPerf({
        job,
        platform,
        dir,
        clice,
        ...(values["no-clangd"]
            ? { clangd: false }
            : values.clangd === undefined
              ? {}
              : { clangd: path.resolve(values.clangd) }),
        ...(values.baseline === undefined ? {} : { baseline: path.resolve(values.baseline) }),
        ...(values.scenario === undefined ? {} : { only: new Set(values.scenario) }),
        dropCaches: values["drop-caches"],
        ...(values.out === undefined ? {} : { out: values.out }),
    });
    write(results, perfReport(results));
    process.exit(results.failures.length === 0 ? 0 : 1);
}
