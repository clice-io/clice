/// Snap-test driver library: everything needed to pin `clice inspect`
/// results over a tests/snap corpus — fixture enumeration, per-fixture
/// execution (spawn, hash handshake, render, snapshot compare) and orphan
/// detection. The vitest glue (tests/snap.test.ts) only enumerates and
/// schedules; concurrency belongs to the test framework.
///
/// Fixtures default to `snap: shared`, where the wire suite
/// (tests/integration/features/snapshots.test.ts) compares the server's
/// replies against the same snapshot file: a mismatch there is a real
/// divergence between the two paths, never something to paper over with
/// an update run.

import * as fs from "node:fs";
import * as path from "node:path";
import { SNAP_DIR } from "../compile_commands.ts";
import { parseAnnotations } from "./annotation.ts";
import {
    OffsetConverter,
    parseFixtureMeta,
    renderRawDocumentLinks,
    renderRawDocumentSymbols,
    renderRawFoldingRanges,
    renderRawInlayHints,
    renderRawSemanticTokens,
    runInspectAsync,
    sha256,
    sortedMarkers,
    type FixtureMeta,
    type InspectFileEntry,
    type RawHoverResult,
} from "./inspect.ts";
import { hoverBlocks } from "./presenters.ts";
import { SnapshotContext } from "./snapshot.ts";

/// CDBs for the tests/snap corpora. Both drivers read the same file:
/// `clice inspect` finds it next to the fixtures and the wire driver
/// initializes its workspace on the corpus directory, so the two paths
/// cannot drift in compile flags.
/// Per-corpus compile flags; everything else defaults to plain c++20.
/// hover pins the target triple because HoverInfo carries sizeof/alignof
/// facts that differ between LP64 and LLP64 hosts.
const CORPUS_FLAGS: Record<string, (corpus: string) => string[]> = {
    document_links: (corpus) => [`-I${posix(corpus)}`, "-std=c++23"],
    hover: () => ["-std=c++20", "--target=x86_64-unknown-linux-gnu"],
};

function posix(p: string): string {
    return p.split(path.sep).join("/");
}

export function generateSnapCDBs(snapDir: string = SNAP_DIR): void {
    if (!fs.existsSync(snapDir)) {
        return;
    }
    for (const corpus of fs.readdirSync(snapDir)) {
        const corpusDir = path.join(snapDir, corpus);
        if (!fs.statSync(corpusDir).isDirectory()) {
            continue;
        }
        const sources = fs
            .readdirSync(corpusDir, { recursive: true, encoding: "utf8" })
            .filter((name) => name.endsWith(".cpp"))
            .sort()
            .map((name) => path.join(corpusDir, name));
        if (sources.length === 0) {
            continue;
        }
        const flags = CORPUS_FLAGS[corpus] ?? (() => ["-std=c++20"]);
        const target = path.join(corpusDir, "compile_commands.json");
        const tmp = `${target}.tmp-${process.pid}`;
        fs.writeFileSync(
            tmp,
            JSON.stringify(
                sources.map((src) => ({
                    directory: posix(corpusDir),
                    file: posix(src),
                    arguments: ["clang++", ...flags(corpusDir), "-fsyntax-only", posix(src)],
                })),
                null,
                2,
            ),
        );
        fs.renameSync(tmp, target);
    }
}

/// Renders one inspect file entry into snapshot lines. Whole-document
/// features read `entry.result`; marker features read `entry.markers`.
type EntryRenderer = (entry: InspectFileEntry, stripped: Buffer, corpus: string) => string[];

function markerSections(
    entry: InspectFileEntry,
    renderOne: (value: unknown) => string[],
): string[] {
    const out: string[] = [];
    for (const [name, value] of sortedMarkers(entry.markers ?? {})) {
        if (out.length > 0) {
            out.push("");
        }
        out.push(`${name}:`);
        out.push(...renderOne(value));
    }
    return out;
}

const RENDERERS: Record<string, EntryRenderer> = {
    document_links: (entry, stripped, corpus) =>
        renderRawDocumentLinks(entry.result, stripped, corpus),
    document_symbol: (entry, stripped) => renderRawDocumentSymbols(entry.result, stripped),
    folding_range: (entry, stripped) => renderRawFoldingRanges(entry.result, stripped),
    hover: (entry, stripped) => {
        const map = new OffsetConverter(stripped);
        const items: [string, { rangeText: string | null; contents: string } | null][] = [];
        for (const [name, value] of sortedMarkers(entry.markers ?? {})) {
            if (value === null) {
                items.push([name, null]);
                continue;
            }
            const hover = value as RawHoverResult;
            let rangeText: string | null = null;
            if (hover.range !== null) {
                const start = map.position(hover.range.begin);
                const end = map.position(hover.range.end);
                rangeText = `${start.line}:${start.character}-${end.line}:${end.character}`;
            }
            items.push([name, { rangeText, contents: hover.contents }]);
        }
        return hoverBlocks(items);
    },
    inlay_hint: (entry, stripped) =>
        entry.markers != null
            ? markerSections(entry, (value) => renderRawInlayHints(value, stripped))
            : renderRawInlayHints(entry.result, stripped),
    semantic_tokens: (entry, stripped) => renderRawSemanticTokens(entry.result, stripped),
};

export interface SnapFixture {
    rel: string;
    content: string;
    meta: FixtureMeta;
    /// False for `status: unsupported` and `snap: skip` fixtures, which
    /// both suites skip and which keep no snapshot.
    active: boolean;
}

export interface SnapCorpus {
    feature: string;
    corpus: string;
    fixtures: SnapFixture[];
}

/// Enumerate the corpora under tests/snap. A corpus directory without a
/// registered renderer would otherwise be silently skipped and its
/// fixtures never run, so that is an error.
export function snapCorpora(): SnapCorpus[] {
    const corpora: SnapCorpus[] = [];
    for (const feature of fs.readdirSync(SNAP_DIR).sort()) {
        const corpus = path.join(SNAP_DIR, feature);
        if (!fs.statSync(corpus).isDirectory()) {
            continue;
        }
        if (!RENDERERS[feature]) {
            throw new Error(`no raw renderer registered for tests/snap/${feature}`);
        }
        const fixtures = fs
            .readdirSync(corpus, { recursive: true, encoding: "utf8" })
            .filter((name) => name.endsWith(".cpp"))
            .sort()
            .map((name) => name.split(path.sep).join("/"))
            .map((rel) => {
                const content = fs.readFileSync(path.join(corpus, rel), "utf8");
                const meta = parseFixtureMeta(content, `${feature}/${rel}`);
                const active = meta.status !== "unsupported" && meta.snap !== "skip";
                return { rel, content, meta, active };
            });
        corpora.push({ feature, corpus, fixtures });
    }
    return corpora;
}

/// Run one fixture end to end: spawn `clice inspect`, verify the C++/TS
/// stripper twins via the content hash, render, and compare against the
/// colocated snapshot. Throws on any failure (including a snapshot
/// mismatch), so a test wrapper needs no extra assertions.
export async function checkSnapFixture(
    clice: string,
    { feature, corpus }: SnapCorpus,
    fixture: SnapFixture,
): Promise<void> {
    const render = RENDERERS[feature];
    if (!render) {
        throw new Error(`no raw renderer registered for tests/snap/${feature}`);
    }
    const file = path.join(corpus, fixture.rel);
    const entry = (await runInspectAsync(clice, feature, file)).files[path.basename(file)];
    if (!entry) {
        throw new Error(`clice inspect returned no entry for ${fixture.rel}`);
    }

    const source = parseAnnotations(fixture.content);
    const stripped = Buffer.from(source.content);
    // Hash equality proves the C++ and TS annotation strippers still agree
    // on the coordinate space of every offset below.
    if (entry.stripped_hash !== sha256(stripped)) {
        throw new Error(
            `${feature}/${fixture.rel}: stripped-content hash mismatch: ` +
                "C++/TS stripper twins have drifted",
        );
    }

    if (entry.error) {
        // An active fixture that does not compile is a failing test, never
        // snapshot content: pinning a marker would let an update run
        // silently accept a transient toolchain or compile failure.
        const diagnostics = (entry.diagnostics ?? []).join("\n  ");
        throw new Error(
            `${feature}/${fixture.rel}: clice inspect failed (${entry.error})` +
                (diagnostics ? `\n  ${diagnostics}` : ""),
        );
    }

    const snapshots = new SnapshotContext(corpus, { colocated: true });
    snapshots.check(fixture.rel, render(entry, stripped, corpus).join("\n"));
}

/// Snapshots follow their sources: a stale `.snap.yml` whose fixture was
/// renamed, deleted, marked unsupported/skip — or a `.wire` variant left
/// behind after a separate fixture went shared — must not linger as if it
/// still pinned anything.
export function orphanSnapshots({ corpus, fixtures }: SnapCorpus): string[] {
    const allowed = new Set<string>();
    for (const fixture of fixtures) {
        if (!fixture.active) {
            continue;
        }
        const base = fixture.rel.replace(/\.cpp$/, "");
        allowed.add(`${base}.snap.yml`);
        if (fixture.meta.snap === "separate") {
            allowed.add(`${base}.wire.snap.yml`);
        }
    }
    return fs
        .readdirSync(corpus, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".snap.yml"))
        .map((name) => name.split(path.sep).join("/"))
        .filter((rel) => !allowed.has(rel));
}
