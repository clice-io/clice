/// A job's compilation database: the project's own, or the entries of the
/// job's part written beside it with a clice.toml that names them, so clice
/// loads that one database and no other the tree happens to hold.

import * as fs from "node:fs";
import * as path from "node:path";
import { ending, runProcess } from "../client/process.ts";
import { cliceExecutable } from "../client/session.ts";
import {
    entryArguments,
    entrySource,
    withoutOutputs,
    type DatabaseEntry,
} from "../compat/database.ts";
import type { Job } from "./projects.ts";

/// Where a job's own database goes, checkout-relative.
const JOB_DATABASE = ".nightly/compile_commands.json";

/// The parts whose longest prefix holds `file`: a part may take a
/// directory from inside another's, as a subproject its generated sources.
function closestParts(parts: Record<string, string[]>, checkout: string, file: string): string[] {
    const relative = path.relative(checkout, file).split(path.sep).join("/");
    const holding = Object.entries(parts).flatMap(([name, prefixes]) =>
        prefixes
            .filter((prefix) => relative.startsWith(prefix))
            .map((prefix) => ({ name, length: prefix.length })),
    );
    const longest = Math.max(...holding.map((part) => part.length));
    return holding.filter((part) => part.length === longest).map((part) => part.name);
}

/// The part of a unit: every unit is in exactly one.
function partOf(parts: Record<string, string[]>, checkout: string, source: string): string {
    const closest = closestParts(parts, checkout, source);
    const [only] = closest;
    if (closest.length !== 1 || only === undefined) {
        const relative = path.relative(checkout, source).split(path.sep).join("/");
        throw new Error(
            `${relative} is in ${closest.length === 0 ? "no part" : `the parts ${closest.join(", ")}`}: the parts must divide the database`,
        );
    }
    return only;
}

/// Whether a checkout-relative file is the job's: any file of a project in
/// one piece, else one its part alone holds.
export function inJob(job: Job, checkout: string, file: string): boolean {
    const { parts } = job.project;
    if (parts === undefined) {
        return true;
    }
    const closest = closestParts(parts, checkout, path.join(checkout, file));
    return closest.length === 1 && closest[0] === job.part;
}

/// The job's units, and the database clice reads them from in place.
export function jobDatabase(job: Job, checkout: string): DatabaseEntry[] {
    const entries = JSON.parse(
        fs.readFileSync(path.join(checkout, job.project.database), "utf8"),
    ) as DatabaseEntry[];
    const { parts } = job.project;
    if (parts === undefined && job.project.database === "compile_commands.json") {
        return entries;
    }
    const own =
        parts === undefined
            ? entries
            : entries.filter((entry) => partOf(parts, checkout, entrySource(entry)) === job.part);
    fs.mkdirSync(path.join(checkout, path.dirname(JOB_DATABASE)), { recursive: true });
    fs.writeFileSync(path.join(checkout, JOB_DATABASE), JSON.stringify(own, null, 1));
    fs.writeFileSync(
        path.join(checkout, "clice.toml"),
        `[[rules]]\ncompile_commands = ["${JOB_DATABASE}"]\n`,
    );
    return own;
}

/// The checkout-relative path of a file inside the checkout; null for one
/// outside it.
export function inside(checkout: string, file: string): string | null {
    const real = fs.existsSync(file) ? fs.realpathSync(file) : file;
    const relative = path.relative(fs.realpathSync(checkout), real);
    return relative.startsWith("..") || path.isAbsolute(relative)
        ? null
        : relative.split(path.sep).join("/");
}

/// Hang detection: the bound only a clice scan or query that hangs reaches.
const QUERY_MS = 30 * 60_000;

/// The JSON a clice subcommand prints over the checkout.
async function cliceJson<T>(checkout: string, args: string[]): Promise<T> {
    const ran = await runProcess(cliceExecutable(), [...args, "--workspace", checkout], {
        timeout: QUERY_MS,
    });
    if (ran.status !== 0) {
        throw new Error(`clice ${args.join(" ")} ${ending(ran)}:\n${ran.stdout}\n${ran.stderr}`);
    }
    return JSON.parse(ran.stdout) as T;
}

/// The checkout-relative headers inside `checkout` the units include
/// themselves, each with the first unit including it: the headers a reader
/// of those units opens. clice's scan finds every include, those of the
/// branches the build leaves out too (another platform's headers); its
/// index holds the files the build's compiles reached. The headers of the
/// clice under test are the compiler's, not the project's: its resource
/// directory lies in the checkout when clice is the project.
export async function includedHeaders(
    checkout: string,
    clice: string,
): Promise<Map<string, string>> {
    const { files } = await cliceJson<{
        files: { path: string; source: boolean; includes: string[] }[];
    }>(checkout, ["scan", "--json"]);
    const indexed = await cliceJson<{ result: { files: { path: string }[] } }>(checkout, [
        "query",
        "project-files",
        "--filter",
        "header",
    ]);
    const reached = new Set(indexed.result.files.map((file) => file.path));
    const programs = path.dirname(path.dirname(fs.realpathSync(clice)));
    const headers = new Map<string, string>();
    for (const unit of files.filter((file) => file.source)) {
        for (const included of unit.includes) {
            const header = inside(checkout, included);
            if (
                header !== null &&
                reached.has(included) &&
                inside(programs, included) === null &&
                !headers.has(header)
            ) {
                headers.set(header, unit.path);
            }
        }
    }
    return headers;
}

/// The entry's command checking the code only: what the build ran, without
/// the files it writes, and `-fsyntax-only`. A CMake precompiled header is
/// read from its source instead (the `-include` beside it stays): the
/// preparation builds no `.pch`.
export function syntaxOnly(entry: DatabaseEntry): string[] {
    const args = withoutOutputs(entryArguments(entry));
    const pch = args.findIndex((arg, i) => arg === "-Xclang" && args[i + 1] === "-include-pch");
    if (pch !== -1) {
        args.splice(pch, 4);
    }
    return [...args, "-fsyntax-only"];
}
