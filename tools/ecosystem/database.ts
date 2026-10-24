/// A job's compilation database: the configuration's own, or the entries of
/// the job's part (or the configuration's thinned database) written beside
/// it with a clice.toml that names them, so clice loads that one database
/// and no other the tree happens to hold.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { ending, runProcess } from "../client/process.ts";
import { cliceExecutable } from "../client/session.ts";
import {
    entryArguments,
    entrySource,
    isMsvc,
    isNvcc,
    withoutOutputs,
    type DatabaseEntry,
} from "../compat/database.ts";
import type { Configuration } from "./project.ts";

/// Where a job's own database goes, checkout-relative.
const JOB_DATABASE = ".ecosystem/compile_commands.json";

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

/// Whether a checkout-relative file is the part's: any file of a database
/// in one piece, else one its part alone holds.
export function inPart(
    config: Configuration,
    part: string | undefined,
    checkout: string,
    file: string,
): boolean {
    const { parts } = config;
    if (parts === undefined) {
        return true;
    }
    const closest = closestParts(parts, checkout, path.join(checkout, file));
    return closest.length === 1 && closest[0] === part;
}

/// `count` of the items, spread evenly over them: all when there are no
/// more.
export function spread<T>(items: readonly T[], count: number): T[] {
    if (items.length <= count) {
        return [...items];
    }
    return Array.from(
        { length: count },
        (_, i) => items[Math.floor(((i + 0.5) * items.length) / count)] as T,
    );
}

/// `keep` entries spread evenly over the database in path order.
export function thinned(entries: readonly DatabaseEntry[], keep: number): DatabaseEntry[] {
    return spread(
        [...entries].sort((a, b) => (entrySource(a) < entrySource(b) ? -1 : 1)),
        keep,
    );
}

export interface JobDatabase {
    entries: DatabaseEntry[];
    /// The directory holding the compile_commands.json clice reads.
    directory: string;
}

/// The units of a configuration, or of one of its parts, and the database
/// clice reads them from in place.
export function jobDatabase(
    config: Configuration,
    part: string | undefined,
    checkout: string,
): JobDatabase {
    const entries = JSON.parse(
        fs.readFileSync(path.join(checkout, config.database), "utf8"),
    ) as DatabaseEntry[];
    const { parts, keep } = config;
    const whole = parts === undefined || part === undefined;
    if (whole && keep === undefined && config.database === "compile_commands.json") {
        return { entries, directory: checkout };
    }
    const own =
        parts !== undefined && part !== undefined
            ? entries.filter((entry) => partOf(parts, checkout, entrySource(entry)) === part)
            : keep !== undefined
              ? thinned(entries, keep)
              : entries;
    fs.mkdirSync(path.join(checkout, path.dirname(JOB_DATABASE)), { recursive: true });
    fs.writeFileSync(path.join(checkout, JOB_DATABASE), JSON.stringify(own, null, 1));
    fs.writeFileSync(
        path.join(checkout, "clice.toml"),
        `[[rules]]\ncompile_commands = ["${JOB_DATABASE}"]\n`,
    );
    return { entries: own, directory: path.join(checkout, path.dirname(JOB_DATABASE)) };
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

/// Every file clice's scan of the checkout reaches, each with what it
/// includes: absolute paths.
export async function scanFiles(
    checkout: string,
): Promise<{ path: string; source: boolean; includes: string[] }[]> {
    const { files } = await cliceJson<{
        files: { path: string; source: boolean; includes: string[] }[];
    }>(checkout, ["scan", "--json"]);
    return files;
}

/// The checkout-relative headers inside `checkout` the units include
/// themselves, each with the first unit including it: the headers a reader
/// of those units opens. clice's scan finds every include, those of the
/// branches the build leaves out too (another platform's headers); its
/// index, once `indexed`, holds the files the build's compiles reached. The
/// headers of the clice under test are the compiler's, not the project's:
/// its resource directory lies in the checkout when clice is the project.
export async function includedHeaders(
    checkout: string,
    clice: string,
    indexed: boolean,
): Promise<Map<string, string>> {
    const files = await scanFiles(checkout);
    const reached = indexed
        ? new Set(
              (
                  await cliceJson<{ result: { files: { path: string }[] } }>(checkout, [
                      "query",
                      "project-files",
                      "--filter",
                      "header",
                  ])
              ).result.files.map((file) => file.path),
          )
        : undefined;
    const resources = path.join(path.dirname(fs.realpathSync(clice)), "..", "lib");
    const headers = new Map<string, string>();
    for (const unit of files.filter((file) => file.source)) {
        for (const included of unit.includes) {
            const header = inside(checkout, included);
            if (
                header !== null &&
                (reached?.has(included) ?? true) &&
                inside(resources, included) === null &&
                !headers.has(header)
            ) {
                headers.set(header, unit.path);
            }
        }
    }
    return headers;
}

/// The entry's command checking the code only: what the build ran, without
/// the files it writes, and `-fsyntax-only` (`/Zs` for cl; nvcc has no such
/// mode, and compiles into the null device). A CMake precompiled header is
/// read from its source instead (the forced include beside it stays): the
/// preparation builds no `.pch`.
export function syntaxOnly(entry: DatabaseEntry): string[] {
    const args = withoutOutputs(entryArguments(entry));
    if (isNvcc(args[0] ?? "")) {
        return [...args, "-c", "-o", os.devNull];
    }
    if (isMsvc(args[0] ?? "")) {
        const kept = args.filter((arg) => !/^[/-](Yu|Yc|Fp)/.test(arg));
        // Options stay before a `--` that ends them (CMake writes one before
        // the source for clang-cl).
        const end = kept.indexOf("--");
        kept.splice(end === -1 ? kept.length : end, 0, "/Zs");
        return kept;
    }
    const pch = args.findIndex((arg, i) => arg === "-Xclang" && args[i + 1] === "-include-pch");
    if (pch !== -1) {
        args.splice(pch, 4);
    }
    return [...args, "-fsyntax-only"];
}

/// A deterministic shuffle: the same seed picks the same files.
export function shuffled<T>(items: readonly T[], seed: string): T[] {
    let state = 2166136261;
    for (let i = 0; i < seed.length; i += 1) {
        state = Math.imul(state ^ seed.charCodeAt(i), 16777619) >>> 0;
    }
    const random = () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    const out = [...items];
    for (let i = out.length - 1; i > 0; i -= 1) {
        const j = Math.floor(random() * (i + 1));
        [out[i], out[j]] = [out[j] as T, out[i] as T];
    }
    return out;
}

function sampled(files: readonly string[], kind: RegExp, count: number, seed: string): string[] {
    const candidates = [...new Set(files.filter((file) => kind.test(file)))].sort();
    return shuffled(candidates, seed).slice(0, count).sort();
}

const SOURCE = /\.(c|cc|cpp|cxx|c\+\+|cppm|ixx|cu)$/;

/// `count` of the units' source files, the seed choosing which.
export function sampleFiles(units: readonly string[], count: number, seed: string): string[] {
    return sampled(units, SOURCE, count, seed);
}

/// `count` of the headers among `files`, the seed choosing which: the
/// server compiles one in the context of a source that includes it, and
/// some only compile there.
export function sampleHeaders(files: readonly string[], count: number, seed: string): string[] {
    return sampled(files, /\.(h|hh|hpp|hxx)$/, count, seed);
}
