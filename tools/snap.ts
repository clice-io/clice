/// Standalone snap-test runner: drives `clice inspect` over the tests/snap
/// corpora — no server involved — and pins the rendered payloads next to
/// their sources, replay.ts-style. One inspect process per fixture, fanned
/// out through p-limit up to the core count, so the suite stays parallel
/// without a test framework around it.
///
/// Usage: node tools/snap.ts --clice build/RelWithDebInfo/bin/clice [--update]
///
/// Fixtures default to `snap: shared`, where the wire suite
/// (tests/integration/features/snapshots.test.ts) compares the server's
/// replies against the same snapshot file: a mismatch there is a real
/// divergence between the two paths, never something to paper over with
/// an update run.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import pLimit from "p-limit";
import { generateSnapCDBs, SNAP_DIR } from "./compile_commands.ts";
import { parseAnnotations } from "./snapshot/annotation.ts";
import {
    parseFixtureMeta,
    renderRawFoldingRanges,
    renderRawSemanticTokens,
    runInspectAsync,
    sha256,
    type FixtureMeta,
    type InspectFileEntry,
    type RawRenderer,
} from "./snapshot/inspect.ts";
import { SnapshotContext } from "./snapshot/snapshot.ts";

const RENDERERS: Record<string, RawRenderer> = {
    folding_range: renderRawFoldingRanges,
    semantic_tokens: renderRawSemanticTokens,
};

function usageError(message: string): never {
    console.error(message);
    console.error("usage: snap.ts --clice CLICE [--update]");
    process.exit(2);
}

function parseRunnerArgs(): { clice: string; update: boolean } {
    let parsed;
    try {
        parsed = parseArgs({
            options: {
                clice: { type: "string" },
                update: { type: "boolean", default: false },
            },
        });
    } catch (error) {
        usageError(error instanceof Error ? error.message : String(error));
    }
    const clice = parsed.values.clice;
    if (clice === undefined) {
        usageError("argument --clice is required");
    }
    if (!fs.existsSync(clice)) {
        usageError(`clice executable not found at '${clice}'`);
    }
    const update = parsed.values.update || process.env["UPDATE_SNAPSHOTS"] === "1";
    return { clice: path.resolve(clice), update };
}

interface Job {
    feature: string;
    rel: string;
    content: string;
    meta: FixtureMeta;
    entry?: InspectFileEntry | undefined;
    spawnError?: string | undefined;
}

async function inspectAll(clice: string, jobs: Job[]): Promise<void> {
    const limit = pLimit(Math.max(1, os.availableParallelism()));
    await Promise.all(
        jobs.map((job) =>
            limit(async () => {
                const file = path.join(SNAP_DIR, job.feature, job.rel);
                try {
                    const output = await runInspectAsync(clice, job.feature, file);
                    job.entry = output.files[path.basename(file)];
                } catch (error) {
                    job.spawnError = error instanceof Error ? error.message : String(error);
                }
            }),
        ),
    );
}

const { clice, update } = parseRunnerArgs();
generateSnapCDBs();

let passed = 0;
let skipped = 0;
const failures: string[] = [];

const fail = (name: string, message: string) => {
    failures.push(name);
    console.error(`FAIL ${name}\n${message}`);
};

const jobs: Job[] = [];
const features: string[] = [];
for (const feature of fs.readdirSync(SNAP_DIR).sort()) {
    const corpus = path.join(SNAP_DIR, feature);
    if (!fs.statSync(corpus).isDirectory()) {
        continue;
    }
    // A corpus directory without a renderer would otherwise be silently
    // skipped and its fixtures never run.
    if (!RENDERERS[feature]) {
        throw new Error(`no raw renderer registered for tests/snap/${feature}`);
    }
    features.push(feature);

    const fixtures = fs
        .readdirSync(corpus, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".cpp"))
        .sort()
        .map((name) => name.split(path.sep).join("/"));
    for (const rel of fixtures) {
        const content = fs.readFileSync(path.join(corpus, rel), "utf8");
        const meta = parseFixtureMeta(content, `${feature}/${rel}`);
        if (meta.status === "unsupported" || meta.snap === "skip") {
            skipped += 1;
            continue;
        }
        jobs.push({ feature, rel, content, meta });
    }
}

await inspectAll(clice, jobs);

for (const feature of features) {
    const corpus = path.join(SNAP_DIR, feature);
    const render = RENDERERS[feature];
    if (!render) {
        continue;
    }
    const snapshots = new SnapshotContext(corpus, { update, colocated: true });
    const allowedSnaps = new Set<string>();

    for (const job of jobs.filter((j) => j.feature === feature)) {
        const name = `${feature}/${job.rel}`;
        const base = job.rel.replace(/\.cpp$/, "");
        allowedSnaps.add(`${base}.snap.yml`);
        if (job.meta.snap === "separate") {
            allowedSnaps.add(`${base}.wire.snap.yml`);
        }

        if (job.spawnError !== undefined || job.entry === undefined) {
            fail(name, job.spawnError ?? "clice inspect returned no entry");
            continue;
        }
        const entry = job.entry;

        const source = parseAnnotations(job.content);
        const stripped = Buffer.from(source.content);
        // Hash equality proves the C++ and TS annotation strippers still
        // agree on the coordinate space of every offset below.
        if (entry.stripped_hash !== sha256(stripped)) {
            fail(name, "stripped-content hash mismatch: C++/TS stripper twins have drifted");
            continue;
        }

        try {
            if (entry.error) {
                // Diagnostics carry machine-dependent paths; pin only the
                // stable marker and surface the details on the console.
                console.error(`[snap] ${name}: ${entry.error}`);
                for (const diag of entry.diagnostics ?? []) {
                    console.error(`[snap]   ${diag}`);
                }
                snapshots.check(job.rel, "COMPILE_ERROR");
            } else {
                snapshots.check(job.rel, render(entry.result, stripped).join("\n"));
            }
            passed += 1;
        } catch (error) {
            fail(name, error instanceof Error ? error.message : String(error));
        }
    }

    // Snapshots follow their sources: a stale `.snap.yml` whose fixture was
    // renamed, deleted, marked unsupported/skip — or a `.wire` variant left
    // behind after a separate fixture went shared — must not linger as if
    // it still pinned anything.
    const orphans = fs
        .readdirSync(corpus, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".snap.yml"))
        .map((name) => name.split(path.sep).join("/"))
        .filter((rel) => !allowedSnaps.has(rel));
    for (const orphan of orphans) {
        fail(`${feature}/${orphan}`, "orphan snapshot: no active fixture pins it");
    }
}

console.log(`${passed} passed, ${skipped} skipped, ${failures.length} failed`);
process.exit(failures.length > 0 ? 1 : 0);
