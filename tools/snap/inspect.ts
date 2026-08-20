/// Inspect-path snap driver: run a feature over a fixture without a server
/// (`clice inspect`, one process per fixture, no PCH, nothing written to
/// disk) and pin the rendered payloads. Raw results carry byte offsets
/// into the annotation-stripped source; every conversion the server
/// performs on the reply edge (position mapping, delta encoding,
/// vocabulary) is re-derived by the feature adapters independently, so a
/// `snap: shared` fixture pins both paths against one file and a
/// divergence fails the suite instead of hiding.

import { execFile, execFileSync } from "node:child_process";
import * as crypto from "node:crypto";
import * as path from "node:path";
import { promisify } from "node:util";
import { resolveFlags, type FixtureFile, type SnapCorpus, type SnapFixture } from "./corpus.ts";
import { feature, participates } from "./registry.ts";
import { abBlocks, fileSections, type InspectFileEntry, type InspectOutput } from "./render.ts";
import { SnapshotContext } from "./snapshot.ts";

export interface InspectInvocation {
    /// Extra compile flags, forwarded as --flags (replacing any CDB).
    flags?: string[] | undefined;
    /// Feature-options overlay, forwarded as --config.
    config?: string | undefined;
}

function inspectArgs(feature: string, target: string, options: InspectInvocation): string[] {
    // --annotations is always passed: these helpers exist to drive fixture
    // corpora, where inline §-markers are part of the grammar. Plain
    // `clice inspect` (no flag) compiles sources verbatim.
    const args = ["inspect", "--annotations", feature, target];
    if (options.flags !== undefined) {
        args.push(`--flags=${JSON.stringify(options.flags)}`);
    }
    if (options.config !== undefined) {
        args.push(`--config=${options.config}`);
    }
    return args;
}

export function runInspect(
    executable: string,
    feature: string,
    target: string,
    options: InspectInvocation = {},
): InspectOutput {
    const stdout = execFileSync(executable, inspectArgs(feature, target, options), {
        encoding: "utf8",
        maxBuffer: 64 * 1024 * 1024,
        timeout: 300_000,
    });
    return JSON.parse(stdout) as InspectOutput;
}

/// Async variant for callers that fan inspect processes out in parallel.
export async function runInspectAsync(
    executable: string,
    feature: string,
    target: string,
    options: InspectInvocation = {},
): Promise<InspectOutput> {
    const { stdout } = await promisify(execFile)(
        executable,
        inspectArgs(feature, target, options),
        {
            encoding: "utf8",
            maxBuffer: 64 * 1024 * 1024,
            timeout: 300_000,
        },
    );
    return JSON.parse(stdout) as InspectOutput;
}

export function sha256(data: Buffer): string {
    return crypto.createHash("sha256").update(data).digest("hex");
}

/// Run one fixture end to end: spawn `clice inspect` on the fixture source
/// (or the unit directory), verify the C++/TS stripper twins via the
/// content hashes, gate on diagnostics, render the participating files,
/// and compare against the colocated snapshot. Throws on any failure
/// (including a snapshot mismatch), so a test wrapper needs no extra
/// assertions.
export async function checkInspectFixture(
    clice: string,
    corpus: SnapCorpus,
    fixture: SnapFixture,
): Promise<void> {
    const { shape, fromInspect } = feature(corpus.feature);
    const target = path.join(corpus.corpus, fixture.unit === "" ? fixture.rel : fixture.unit);
    const flags = [
        ...resolveFlags(corpus.flags, corpus.corpus),
        ...resolveFlags(fixture.meta.flags, corpus.corpus),
    ];

    const inspectEntries = async (config?: string): Promise<[FixtureFile, InspectFileEntry][]> => {
        const output = await runInspectAsync(clice, corpus.feature, target, { flags, config });
        const entries: [FixtureFile, InspectFileEntry][] = [];
        for (const file of fixture.files) {
            const key = fixture.unit === "" ? file.rel : file.rel.slice(fixture.unit.length + 1);
            const entry = output.files[key];
            if (!entry) {
                throw new Error(`clice inspect returned no entry for ${file.rel}`);
            }
            // Hash equality proves the C++ and TS annotation strippers
            // still agree on the coordinate space of every offset below.
            if (entry.stripped_hash !== sha256(Buffer.from(file.source.content))) {
                throw new Error(
                    `${corpus.feature}/${file.rel}: stripped-content hash mismatch: ` +
                        "C++/TS stripper twins have drifted",
                );
            }
            if (entry.error) {
                // An active fixture that does not compile is a failing
                // test, never snapshot content: pinning a marker would let
                // an update run silently accept a transient toolchain or
                // compile failure.
                const diagnostics = (entry.diagnostics ?? []).join("\n  ");
                throw new Error(
                    `${corpus.feature}/${file.rel}: clice inspect failed (${entry.error})` +
                        (diagnostics ? `\n  ${diagnostics}` : ""),
                );
            }
            entries.push([file, entry]);
        }

        // The AST builds even for broken sources, so a compile that
        // "succeeds" may still carry error diagnostics — e.g. a mistyped
        // annotation that swallowed real code. Never pin such a fixture. A
        // fixture that tests behavior on broken code declares it with
        // `- diagnostics: expected`, and then a clean compile means the
        // declaration went stale — both directions fail.
        const diagnostics = entries.flatMap(([, entry]) => entry.diagnostics ?? []);
        if (diagnostics.length > 0 && !fixture.meta.diagnostics) {
            throw new Error(
                `${corpus.feature}/${fixture.rel}: fixture does not compile cleanly:\n  ` +
                    diagnostics.join("\n  "),
            );
        }
        if (diagnostics.length === 0 && fixture.meta.diagnostics) {
            throw new Error(
                `${corpus.feature}/${fixture.rel}: diagnostics: expected, ` +
                    "but the fixture compiled cleanly",
            );
        }
        return entries;
    };

    const render = (entries: [FixtureFile, InspectFileEntry][]): string[] => {
        const sections: [string, string[]][] = [];
        for (const [file, entry] of entries) {
            if (!participates(shape, file.source, file.rel === fixture.rel)) {
                continue;
            }
            const label = fixture.unit === "" ? file.rel : file.rel.slice(fixture.unit.length + 1);
            const stripped = Buffer.from(file.source.content);
            sections.push([
                label,
                fromInspect(entry, { source: file.source, stripped, root: corpus.corpus }),
            ]);
        }
        if (sections.length === 0) {
            throw new Error(`${corpus.feature}/${fixture.rel}: no file carries ${shape} markers`);
        }
        return fileSections(sections);
    };

    // A config fixture pins both halves of the A/B: the run above is the
    // default half, a second inspect run carries the overlay. The compile
    // is identical, so the gates run identically too.
    let body = render(await inspectEntries());
    if (fixture.meta.config !== undefined) {
        body = abBlocks(body, render(await inspectEntries(fixture.meta.config)));
    }

    const variant =
        fixture.meta.verify === "both" && fixture.meta.snap === "separate" ? "inspect" : "";
    const snapshots = new SnapshotContext(corpus.corpus, { colocated: true });
    snapshots.check(fixture.rel, body.join("\n"), variant);
}
