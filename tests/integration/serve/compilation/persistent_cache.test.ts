/// Built PCHs and PCMs live in the versioned cache store ({pch,pcm}/
/// namespaces) under content-addressed names; they outlive the server that
/// built them, a change made while no server runs reaches them, and a store
/// a server left damaged is recovered or rebuilt.

import * as fs from "node:fs";
import * as path from "node:path";
import type { Serve } from "@clice/tools/actions";
import { MTIME_GRANULARITY } from "@clice/tools/client";
import { readManifest } from "@clice/tools/project";
import { expect, serve } from "../../fixtures.ts";

serve("shapes/headers")("pch survives server restart", async ({ s }) => {
    await s.clean("app/main.cpp");
    expect((await s.counts()).pch).toBe(1);
    await s.restart();
    await s.clean("app/main.cpp");
    expect((await s.counts()).pch).toBe(0);
});

/// What main.cpp's compile consumes of the shapes library, and where the
/// cache keeps it.
const ARTIFACT = {
    "shapes/headers": { kind: "pch", cached: (s: Serve) => s.workspace.pchFiles() },
    "shapes/modules": { kind: "pcm", cached: (s: Serve) => s.workspace.pcmFiles() },
} as const;

serve.each(Object.keys(ARTIFACT))("offline edit reaches callers", async ({ s }) => {
    const artifact = ARTIFACT[s.project as keyof typeof ARTIFACT];
    await s.clean("app/main.cpp");
    expect(artifact.cached(s)).not.toEqual([]);

    await s.offline(() => {
        s.disk.edit(s.file("circle"), { replace: "double area(", with: "double surface(" });
    });
    const errors = await s.errors("app/main.cpp");
    expect(
        s.show(errors.map((error) => ({ uri: s.uri("app/main.cpp"), range: error.range }))),
    ).toBe(
        "app/main.cpp: double total = shapes::area(c) + triangle.measure() + shapes_circle_area(1.0);",
    );
    expect(
        (await s.counts())[artifact.kind],
        `the ${artifact.kind} is built again`,
    ).toBeGreaterThan(0);
});

/// Corrupt a blob in place, preserving file size and mtime; returns the
/// corrupted bytes. "garbage" replaces the whole file (caught by reader
/// validation), "middle" flips a span reached only during deserialization
/// (can abort the consuming process instead of failing cleanly).
function corruptPreservingStat(p: string, where = "garbage", span = 4096): Buffer {
    const stat = fs.statSync(p);
    let data = fs.readFileSync(p);
    if (where === "garbage") {
        data = Buffer.alloc(data.length, 0x5a);
    } else {
        const offset = Math.floor(data.length / 2);
        for (let i = offset; i < Math.min(data.length, offset + span); i++) {
            data[i] = data[i]! ^ 0xff;
        }
    }
    fs.writeFileSync(p, data);
    fs.utimesSync(p, stat.atime, stat.mtime);
    return data;
}

serve("shapes/headers")("pch written to cache dir", async ({ s }) => {
    await s.clean(s.file("main"));
    const pchFiles = s.workspace.pchFiles();
    expect(pchFiles.length, "Expected at least one .pch file in the store").toBeGreaterThanOrEqual(
        1,
    );
    // The key (a 32-char xxh3_128bits hex hash) and the build's nonce.
    expect(path.basename(pchFiles[0]!)).toMatch(/^[0-9a-f]{32}-[0-9a-f]{16}\.pch$/);
});

serve("shapes/headers")("pch reused on close reopen", async ({ s }) => {
    await s.clean(s.file("main"));
    const pchAfterFirst = s.workspace.pchFiles();
    expect(pchAfterFirst.length).toBeGreaterThanOrEqual(1);

    s.close(s.file("main"));
    await s.clean(s.file("main"));
    expect(s.workspace.pchFiles(), "PCH file set should be identical after close+reopen").toEqual(
        pchAfterFirst,
    );
    expect((await s.counts()).files[s.file("main")]?.pch).toBe(1);
});

serve("shapes/modules")("pcm offline break drops it", async ({ s }) => {
    // A module broken while the server is down fails its rebuild on
    // restart: its importer reports the import instead of compiling
    // against the PCM of the module's previous interface.
    await s.clean(s.file("circle"));
    await s.offline(() => {
        s.disk.edit(s.file("shape"), {
            after: "export namespace shapes {\n",
            insert: "\nint broken() {\n    return broken_in_shape;\n}\n",
        });
    });
    const errors = await s.errors(s.file("circle"));
    expect(
        errors.map(
            (error) =>
                `${s.show({ uri: s.uri(s.file("circle")), range: error.range })} ${String(error.code)}`,
        ),
    ).toEqual([`${s.file("circle")}: import :shape; err_module_not_found`]);
});

serve("shapes/headers")("shared preamble shares pch", async ({ s }) => {
    await s.clean(s.file("main"));
    await s.clean(s.file("demo"));
    // Content-addressed naming: one preamble, one .pch.
    const pchFiles = s.workspace.pchFiles();
    expect(
        pchFiles.length,
        `Expected exactly 1 PCH file for shared preamble, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(1);
});

serve("shapes/headers")("different preamble different pch", async ({ s }) => {
    await s.clean(s.file("measure"));
    await s.clean(s.file("registry"));
    const pchFiles = s.workspace.pchFiles();
    expect(
        pchFiles.length,
        `Expected 2 PCH files for different preambles, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(2);
});

serve("shapes/headers")("pch rebuilt on header change", async ({ s }) => {
    await s.clean(s.file("main"));
    expect(s.workspace.pchFiles().length).toBeGreaterThanOrEqual(1);

    // The rewrites keep the sizes: only later mtimes tell them apart.
    s.disk.edit(s.file("circle"), { replace: "double area(", with: "double aera(" });
    s.disk.touch(
        s.file("circle"),
        new Date(s.disk.mtime(s.file("circle")).getTime() + MTIME_GRANULARITY),
    );
    s.disk.edit(s.file("main"), { replace: "shapes::area(c)", with: "shapes::aera(c)" });
    s.disk.touch(
        s.file("main"),
        new Date(s.disk.mtime(s.file("main")).getTime() + MTIME_GRANULARITY),
    );
    s.close(s.file("main"));
    await s.sync({ poll: true });
    await s.clean(s.file("main"));
    // The #include lines are the same text, so the preamble hash and the PCH
    // name may stay; the changed dependency rebuilds it either way.
    expect(s.workspace.pchFiles().length).toBeGreaterThanOrEqual(1);
    expect((await s.counts()).files[s.file("main")]?.pch).toBe(2);
});

serve("shapes/headers")("no tmp files after build", async ({ s }) => {
    await s.clean(s.file("main"));
    await s.sync();
    expect(s.workspace.tmpFiles(), "in-flight tmp files drained").toEqual([]);
    // The pch namespace legitimately holds the paired .pch.idx blobs.
    const expected: Record<string, string[]> = { pch: [".pch", ".pch.idx"], pcm: [".pcm"] };
    for (const [subdir, extensions] of Object.entries(expected)) {
        const blobDir = path.join(s.workspace.cacheRoot(), subdir);
        if (fs.existsSync(blobDir)) {
            const stray = fs
                .readdirSync(blobDir)
                .filter((name) => !extensions.some((e) => name.endsWith(e)));
            expect(stray, `Stray files in ${subdir}/: ${stray.join(", ")}`).toEqual([]);
        }
    }
});

serve("tiny")("cache dirs created on startup", async ({ s }) => {
    // A compile: load_workspace() runs asynchronously after initialize.
    await s.clean("main.cpp");
    const root = s.workspace.cacheRoot();
    for (const subdir of ["pch", "pcm"]) {
        expect(
            fs.existsSync(path.join(root, subdir)) &&
                fs.statSync(path.join(root, subdir)).isDirectory(),
            `${subdir}/ should be created`,
        ).toBe(true);
    }
    // The index persists into a single LMDB database per configuration,
    // not a namespace dir.
    const library = s.workspace.indexLibrary();
    expect(library, "the anonymous configuration's library").toBeDefined();
    expect(fs.existsSync(path.join(library!, "index.mdb")), "index.mdb should be created").toBe(
        true,
    );
});

const FAST_DEMO = {
    units: { [readManifest("shapes/headers").files!["demo"]!]: ["-DSHAPES_FAST=1"] },
};

serve("shapes/headers", FAST_DEMO)("different flags different pch", async ({ s }) => {
    // main and demo share their preamble; the flag flips config.h's branch.
    await s.clean(s.file("main"));
    await s.clean(s.file("demo"));
    const pchFiles = s.workspace.pchFiles();
    expect(
        pchFiles.length,
        `Same preamble with different -D flags must produce 2 PCHs, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(2);
});

serve("shapes/headers")("kill9 recovery", async ({ s }) => {
    // kill -9 while the PCH build's reply is parked: the worker wrote the
    // pair to tmp, the store has not committed it. The restarted server
    // sweeps the residue and serves the file normally.
    const hold = await s.hold("pch", s.file("main"));
    s.open(s.file("main"));
    await hold.reached();
    expect(s.workspace.tmpFiles(), "the uncommitted pair").not.toEqual([]);
    await s.kill();

    await s.start();
    await s.clean(s.file("main"));
    const pchFiles = s.workspace.pchFiles();
    expect(pchFiles.length, "PCH should be (re)built after crash").toBeGreaterThanOrEqual(1);
    // Blob directories contain only committed blobs (the PCH and its
    // paired index), never partial writes.
    const stray = fs
        .readdirSync(path.join(s.workspace.cacheRoot(), "pch"))
        .filter((name) => !name.endsWith(".pch") && !name.endsWith(".pch.idx"));
    expect(stray, `Crash residue in pch/: ${stray.join(", ")}`).toEqual([]);
    await s.noAnomaly();
    await s.stop();

    // A clean shutdown removes the server's own tmp; the killed one's was
    // swept at startup, so nothing may remain.
    expect(s.workspace.tmpFiles(), "tmp residue should be swept").toEqual([]);
});

for (const where of ["garbage", "middle"]) {
    // The body gets a real error: a bricked file publishes an empty list
    // instead, so the error is the recovery signal. The middle shape may
    // crash a worker, an intentional anomaly.
    serve("shapes/headers", { anomalies: where === "middle" })(
        `corrupt pch rebuilt on restart ${where}`,
        async ({ s }) => {
            // A .pch corrupted offline (size and mtime preserved, so
            // freshness checks pass) must not brick the file: the
            // consumption failure retracts the pair and rebuilds it, and
            // real diagnostics come back.
            s.disk.edit(s.file("main"), {
                replace: "shapes_circle_area(1.0)",
                with: "undeclared_symbol",
            });
            expect(
                await s.errors(s.file("main")),
                "Baseline session should report the body error",
            ).not.toEqual([]);
            expect(s.workspace.pchFiles().length).toBe(1);
            await s.noAnomaly();

            let corrupted = Buffer.alloc(0);
            await s.offline(() => {
                corrupted = corruptPreservingStat(s.workspace.pchFiles()[0]!, where);
            });
            const errors = await s.errors(s.file("main"));
            // The specific body error, not just any error: a quarantine
            // notice or a still-standing corruption fatal must not count as
            // recovery.
            expect(
                errors.some((d) =>
                    (typeof d.message === "string" ? d.message : d.message.value).includes(
                        "undeclared_symbol",
                    ),
                ),
                `Real diagnostics must recover, got: ${JSON.stringify(errors)}`,
            ).toBe(true);
            const pchFiles = s.workspace.pchFiles();
            expect(pchFiles.length, "The pair must be rebuilt, not abandoned").toBe(1);
            if (where === "middle" && fs.readFileSync(pchFiles[0]!).equals(corrupted)) {
                // The flip landed in semantically dead bytes on this LLVM
                // build (PCH blobs carry no whole-file checksum): the reader
                // consumed the blob untouched and there is nothing to heal.
                expect(
                    (await s.counts()).files[s.file("main")]?.pch,
                    "the blob was consumed as it was",
                ).toBe(0);
                return;
            }
            expect(
                fs.readFileSync(pchFiles[0]!).equals(corrupted),
                "The corrupt .pch must be rebuilt, not trusted forever",
            ).toBe(false);
            if (where === "garbage") {
                await s.noAnomaly();
            }
        },
    );
}

serve("shapes/headers")("corrupt pch idx retracted", async ({ s }) => {
    // A corrupt .pch.idx detected at load must retract the on-disk pair
    // (not just the in-memory path): a pair that looks complete would be
    // re-adopted and silently degrade every later session.
    await s.clean(s.file("main"));
    expect(s.workspace.pchIdxFiles().length).toBe(1);
    await s.noAnomaly();

    let corrupted = Buffer.alloc(0);
    await s.offline(() => {
        corrupted = corruptPreservingStat(s.workspace.pchIdxFiles()[0]!);
    });
    // Recovery rides the .pch artifact gate: detecting the corrupt idx
    // retracts the whole pair from the store mid-round, the compile then
    // setup-fails on the now-missing .pch, and the gate rebuilds both.
    await s.clean(s.file("main"));
    const idxFiles = s.workspace.pchIdxFiles();
    expect(idxFiles.length, "The pair must be rebuilt after idx corruption").toBe(1);
    expect(
        fs.readFileSync(idxFiles[0]!).equals(corrupted),
        "The corrupt .pch.idx must be retracted and rebuilt, not left posing as a complete pair",
    ).toBe(false);
    await s.noAnomaly();
});

/// Best-effort recursive wipe: a live server keeps its LMDB index
/// memory-mapped, and Windows refuses to delete mapped files (EPERM) —
/// exactly what a user wiping the cache mid-session experiences there.
/// Everything unlocked still goes.
function wipeBestEffort(dir: string): void {
    let entries: fs.Dirent[];
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
        return;
    }
    for (const entry of entries) {
        const child = path.join(dir, entry.name);
        try {
            if (entry.isDirectory()) {
                wipeBestEffort(child);
                fs.rmdirSync(child);
            } else {
                fs.rmSync(child, { force: true });
            }
        } catch {
            // Locked by the live server; leave it.
        }
    }
}

serve("shapes/headers")("cache wiped while running", async ({ s }) => {
    // Wiping the cache directory under a running server must not wedge
    // PCH builds forever: the store re-creates its directories on demand.
    await s.clean(s.file("main"));
    wipeBestEffort(s.workspace.path(path.join(".clice", "cache")));

    // A preamble change: a fresh PCH build is required.
    s.disk.edit(s.file("circle"), {
        after: "double diameter() const;",
        insert: "\n\n    double weight() const;",
    });
    await s.sync({ poll: true });
    await s.clean(s.file("main"));
    expect(s.workspace.pchFiles().length, "PCH build must recover after a cache wipe").toBe(1);
});
