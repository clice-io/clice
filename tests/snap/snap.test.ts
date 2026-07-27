/// Standalone feature snapshot suite: drives `clice inspect` over the
/// tests/snap corpora — no server involved — and pins the rendered
/// payloads next to their sources. Fixtures default to `snap: shared`,
/// where the wire suite (tests/integration/features/snapshots.test.ts)
/// compares the server's replies against the same file: a mismatch there
/// is a real divergence between the two paths, never something to paper
/// over with UPDATE_SNAPSHOTS.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { beforeAll, describe, expect, test } from "vitest";
import { parseAnnotations } from "@clice/tools/annotation";
import { generateSnapCDBs, SNAP_DIR } from "@clice/tools/compile-commands";
import {
    parseFixtureMeta,
    renderRawFoldingRanges,
    renderRawSemanticTokens,
    runInspect,
    sha256,
    type InspectFileEntry,
    type RawRenderer,
} from "@clice/tools/inspect";
import { SnapshotContext } from "@clice/tools/snapshot";
import { cliceExecutable } from "../fixtures.ts";

const RENDERERS: Record<string, RawRenderer> = {
    folding_range: renderRawFoldingRanges,
    semantic_tokens: renderRawSemanticTokens,
};

function fixturesIn(dir: string): string[] {
    return fs
        .readdirSync(dir, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".cpp"))
        .sort()
        .map((name) => name.split(path.sep).join("/"));
}

for (const feature of fs.readdirSync(SNAP_DIR).sort()) {
    const corpus = path.join(SNAP_DIR, feature);
    if (!fs.statSync(corpus).isDirectory()) {
        continue;
    }
    // A corpus directory without a renderer would otherwise be silently
    // skipped and its fixtures never run.
    const render = RENDERERS[feature];
    if (!render) {
        throw new Error(`no raw renderer registered for tests/snap/${feature}`);
    }

    describe(`snap/${feature}`, () => {
        const fixtures = fixturesIn(corpus);
        let entries: Record<string, InspectFileEntry> = {};

        beforeAll(() => {
            generateSnapCDBs();
            entries = runInspect(cliceExecutable(), feature, corpus).files;
        });

        for (const rel of fixtures) {
            const content = fs.readFileSync(path.join(corpus, rel), "utf8");
            const meta = parseFixtureMeta(content, `${feature}/${rel}`);

            test.skipIf(meta.status === "unsupported" || meta.snap === "skip")(
                `${feature}/${rel}`,
                () => {
                    const entry = entries[rel];
                    if (!entry) {
                        throw new Error(`clice inspect returned no entry for ${rel}`);
                    }

                    const source = parseAnnotations(content);
                    const stripped = Buffer.from(source.content);
                    // Hash equality proves the C++ and TS annotation strippers
                    // still agree on the coordinate space of every offset below.
                    expect(entry.stripped_hash).toBe(sha256(stripped));

                    const snapshots = new SnapshotContext(corpus, { colocated: true });
                    if (entry.error) {
                        // Diagnostics carry machine-dependent paths; pin only
                        // the stable marker and surface details on the console.
                        console.error(`[snap] ${feature}/${rel}: ${entry.error}`);
                        for (const diag of entry.diagnostics ?? []) {
                            console.error(`[snap]   ${diag}`);
                        }
                        snapshots.check(rel, "COMPILE_ERROR");
                        return;
                    }
                    snapshots.check(rel, render(entry.result, stripped).join("\n"));
                },
            );
        }

        // Snapshots follow their sources: a stale `.snap.yml` whose fixture
        // was renamed, deleted, marked unsupported/skip — or a `.wire`
        // variant left behind after a separate fixture went shared — must
        // not linger as if it still pinned anything.
        test("no orphan snapshots", () => {
            const allowed = new Set<string>();
            for (const rel of fixtures) {
                const content = fs.readFileSync(path.join(corpus, rel), "utf8");
                const meta = parseFixtureMeta(content, rel);
                if (meta.status === "unsupported" || meta.snap === "skip") {
                    continue;
                }
                const base = rel.replace(/\.cpp$/, "");
                allowed.add(`${base}.snap.yml`);
                if (meta.snap === "separate") {
                    allowed.add(`${base}.wire.snap.yml`);
                }
            }
            const orphans = fs
                .readdirSync(corpus, { recursive: true, encoding: "utf8" })
                .filter((name) => name.endsWith(".snap.yml"))
                .map((name) => name.split(path.sep).join("/"))
                .filter((rel) => !allowed.has(rel));
            expect(orphans).toEqual([]);
        });
    });
}

// The corpus runs always pass a directory with a generated CDB; pin the
// other two documented modes — a single-file argument and the default-flags
// fallback when no compile_commands.json exists anywhere above the input.
test("single file without CDB", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-inspect-"));
    try {
        const file = path.join(tmp, "single.cpp");
        fs.copyFileSync(path.join(SNAP_DIR, "folding_range", "block_folding.cpp"), file);
        const { files } = runInspect(cliceExecutable(), "folding_range", file);
        const entry = files["single.cpp"];
        expect(entry?.error ?? null).toBeNull();
        const result = entry?.result;
        expect(Array.isArray(result) && result.length > 0).toBe(true);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});
