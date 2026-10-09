/// Built PCHs and PCMs live in the versioned cache store ({pch,pcm}/
/// namespaces) under content-addressed names; they outlive the server that
/// built them, a change made while no server runs reaches them, and a store
/// a server left damaged is recovered or rebuilt.

import * as fs from "node:fs";
import * as path from "node:path";
import type { Serve } from "@clice/tools/actions";
import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
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

serve.files({
    "header.h": "#pragma once\nstruct Foo { int x; };\n",
    "main.cpp": '#include "header.h"\nint main() { Foo f; return f.x; }\n',
})("pch written to cache dir", async ({ s }) => {
    await s.clean("main.cpp");
    const pchFiles = s.workspace.pchFiles();
    expect(pchFiles.length, "Expected at least one .pch file in the store").toBeGreaterThanOrEqual(
        1,
    );
    // The key (a 32-char xxh3_128bits hex hash) and the build's nonce.
    expect(path.basename(pchFiles[0]!)).toMatch(/^[0-9a-f]{32}-[0-9a-f]{16}\.pch$/);
});

serve.files({
    "header.h": "#pragma once\nstruct Bar { int y; };\n",
    "main.cpp": '#include "header.h"\nint main() { Bar b; return b.y; }\n',
})("pch reused on close reopen", async ({ s }) => {
    await s.clean("main.cpp");
    const pchAfterFirst = s.workspace.pchFiles();
    expect(pchAfterFirst.length).toBeGreaterThanOrEqual(1);

    s.close("main.cpp");
    await s.clean("main.cpp");
    expect(s.workspace.pchFiles(), "PCH file set should be identical after close+reopen").toEqual(
        pchAfterFirst,
    );
    expect((await s.counts()).pch).toBe(1);
});

serve.data("modules/save_recompile")("pcm offline break drops it", async ({ s }) => {
    // A module broken while the server is down fails its rebuild on
    // restart: its importer reports the import instead of compiling
    // against the PCM of the module's previous interface.
    await s.clean("mid.cppm");
    await s.offline(() => {
        s.disk.write(
            "leaf.cppm",
            "export module Leaf;\n\nexport int leaf() {\n    return broken_in_leaf;\n}\n",
        );
    });
    expect(
        (await s.errors("mid.cppm")).map(
            (diagnostic) => `${diagnostic.range.start.line} ${String(diagnostic.code)}`,
        ),
    ).toEqual(["1 err_module_not_found"]);
});

serve.files({
    "header.h": "#pragma once\nint shared_val = 1;\n",
    "a.cpp": '#include "header.h"\nint fa() { return shared_val; }\n',
    "b.cpp": '#include "header.h"\nint fb() { return shared_val + 1; }\n',
})("shared preamble shares pch", async ({ s }) => {
    await s.clean("a.cpp");
    await s.clean("b.cpp");
    // Content-addressed naming: one preamble, one .pch.
    const pchFiles = s.workspace.pchFiles();
    expect(
        pchFiles.length,
        `Expected exactly 1 PCH file for shared preamble, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(1);
});

serve.files({
    "a.h": "#pragma once\nint val_a = 1;\n",
    "b.h": "#pragma once\nint val_b = 2;\n",
    "a.cpp": '#include "a.h"\nint fa() { return val_a; }\n',
    "b.cpp": '#include "b.h"\nint fb() { return val_b; }\n',
})("different preamble different pch", async ({ s }) => {
    await s.clean("a.cpp");
    await s.clean("b.cpp");
    const pchFiles = s.workspace.pchFiles();
    expect(
        pchFiles.length,
        `Expected 2 PCH files for different preambles, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(2);
});

serve.files({
    "header.h": "#pragma once\nstruct V1 { int a; };\n",
    "main.cpp": '#include "header.h"\nint main() { V1 v; return v.a; }\n',
})("pch rebuilt on header change", async ({ s }) => {
    await s.clean("main.cpp");
    expect(s.workspace.pchFiles().length).toBeGreaterThanOrEqual(1);

    // The rewrites keep the sizes: only their timestamps tell them apart.
    await sleep(MTIME_GRANULARITY);
    s.disk.write("header.h", "#pragma once\nstruct V2 { int b; };\n");
    s.disk.write("main.cpp", '#include "header.h"\nint main() { V2 v; return v.b; }\n');
    s.close("main.cpp");
    await s.sync({ poll: true });
    await s.clean("main.cpp");
    // The #include line is the same text, so the preamble hash and the PCH
    // name may stay; the changed dependency rebuilds it either way.
    expect(s.workspace.pchFiles().length).toBeGreaterThanOrEqual(1);
    expect((await s.counts()).pch).toBe(2);
});

serve.files({
    "header.h": "#pragma once\nint val = 1;\n",
    "main.cpp": '#include "header.h"\nint main() { return val; }\n',
})("no tmp files after build", async ({ s }) => {
    await s.clean("main.cpp");
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

serve.files({ "main.cpp": "int main() { return 0; }\n" })(
    "cache dirs created on startup",
    async ({ s }) => {
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
    },
);

serve.files(
    {
        "header.h":
            "#pragma once\n#ifdef MODE\nstruct Cfg { int mode; };\n#else\nstruct Cfg { int plain; };\n#endif\n",
        "a.cpp": '#include "header.h"\nint use() { Cfg c; return 0; }\n',
        "b.cpp": '#include "header.h"\nint use() { Cfg c; return 0; }\n',
    },
    { manifest: { cxx: ["-std=c++17"], units: { "a.cpp": [], "b.cpp": ["-DMODE=1"] } } },
)("different flags different pch", async ({ s }) => {
    await s.clean("a.cpp");
    await s.clean("b.cpp");
    const pchFiles = s.workspace.pchFiles();
    expect(
        pchFiles.length,
        `Same preamble with different -D flags must produce 2 PCHs, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(2);
});

serve.files({
    "header.h": "#pragma once\nstruct K { int x; };\n",
    "main.cpp": '#include "header.h"\nint main() { K k; return k.x; }\n',
})("kill9 recovery", async ({ s }) => {
    // kill -9 while the PCH build's reply is parked: the worker wrote the
    // pair to tmp, the store has not committed it. The restarted server
    // sweeps the residue and serves the file normally.
    const killed = s.client;
    const hold = await s.hold("pch", "main.cpp");
    const [uri] = killed.open("main.cpp");
    void killed.pullDiagnostics(uri).catch(() => undefined);
    await hold.reached();
    expect(s.workspace.tmpFiles(), "the uncommitted pair").not.toEqual([]);
    killed.killServer();
    await killed.exited;
    killed.dispose();

    await s.start();
    await s.clean("main.cpp");
    const pchFiles = s.workspace.pchFiles();
    expect(pchFiles.length, "PCH should be (re)built after crash").toBeGreaterThanOrEqual(1);
    // Blob directories contain only committed blobs (the PCH and its
    // paired index), never partial writes.
    const stray = fs
        .readdirSync(path.join(s.workspace.cacheRoot(), "pch"))
        .filter((name) => !name.endsWith(".pch") && !name.endsWith(".pch.idx"));
    expect(stray, `Crash residue in pch/: ${stray.join(", ")}`).toEqual([]);
    s.client.assertNoAnomaly();
    await s.stop();

    // A clean shutdown removes the server's own tmp; the killed one's was
    // swept at startup, so nothing may remain.
    expect(s.workspace.tmpFiles(), "tmp residue should be swept").toEqual([]);
});

for (const where of ["garbage", "middle"]) {
    // The body has a real error: a bricked file publishes an empty list
    // instead, so the error is the recovery signal. The middle shape may
    // crash a worker, an intentional anomaly.
    serve.files(
        {
            "header.h": "#pragma once\nint known_func();\n",
            "main.cpp": '#include "header.h"\nint main() { return undeclared_symbol; }\n',
        },
        { anomalies: where === "middle" },
    )(`corrupt pch rebuilt on restart ${where}`, async ({ s }) => {
        // A .pch corrupted offline (size and mtime preserved, so freshness
        // checks pass) must not brick the file: the consumption failure
        // retracts the pair and rebuilds it, and real diagnostics come back.
        expect(
            await s.errors("main.cpp"),
            "Baseline session should report the body error",
        ).not.toEqual([]);
        expect(s.workspace.pchFiles().length).toBe(1);
        s.client.assertNoAnomaly();

        let corrupted = Buffer.alloc(0);
        await s.offline(() => {
            corrupted = corruptPreservingStat(s.workspace.pchFiles()[0]!, where);
        });
        const errors = await s.errors("main.cpp");
        // The specific body error, not just any error: a quarantine notice or
        // a still-standing corruption fatal must not count as recovery.
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
            // The flip landed in semantically dead bytes on this LLVM build
            // (PCH blobs carry no whole-file checksum): the reader consumed
            // the blob untouched and there is nothing to heal.
            expect((await s.counts()).pch, "the blob was consumed as it was").toBe(0);
            return;
        }
        expect(
            fs.readFileSync(pchFiles[0]!).equals(corrupted),
            "The corrupt .pch must be rebuilt, not trusted forever",
        ).toBe(false);
        if (where === "garbage") {
            s.client.assertNoAnomaly();
        }
    });
}

serve.files({
    "header.h": "#pragma once\nstruct Idx { int v; };\n",
    "main.cpp": '#include "header.h"\nint main() { Idx i; return i.v; }\n',
})("corrupt pch idx retracted", async ({ s }) => {
    // A corrupt .pch.idx detected at load must retract the on-disk pair
    // (not just the in-memory path): a pair that looks complete would be
    // re-adopted and silently degrade every later session.
    await s.clean("main.cpp");
    expect(s.workspace.pchIdxFiles().length).toBe(1);
    s.client.assertNoAnomaly();

    let corrupted = Buffer.alloc(0);
    await s.offline(() => {
        corrupted = corruptPreservingStat(s.workspace.pchIdxFiles()[0]!);
    });
    // Recovery rides the .pch artifact gate: detecting the corrupt idx
    // retracts the whole pair from the store mid-round, the compile then
    // setup-fails on the now-missing .pch, and the gate rebuilds both.
    await s.clean("main.cpp");
    const idxFiles = s.workspace.pchIdxFiles();
    expect(idxFiles.length, "The pair must be rebuilt after idx corruption").toBe(1);
    expect(
        fs.readFileSync(idxFiles[0]!).equals(corrupted),
        "The corrupt .pch.idx must be retracted and rebuilt, not left posing as a complete pair",
    ).toBe(false);
    s.client.assertNoAnomaly();
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

serve.files({
    "header.h": "#pragma once\nstruct W { int x; };\n",
    "main.cpp": '#include "header.h"\nint main() { W w; return w.x; }\n',
})("cache wiped while running", async ({ s }) => {
    // Wiping the cache directory under a running server must not wedge
    // PCH builds forever: the store re-creates its directories on demand.
    await s.clean("main.cpp");
    wipeBestEffort(s.workspace.path(path.join(".clice", "cache")));

    // A preamble change: a fresh PCH build is required.
    s.disk.write("header.h", "#pragma once\nstruct W { int x; int y; };\n");
    await s.sync({ poll: true });
    await s.clean("main.cpp");
    expect(s.workspace.pchFiles().length, "PCH build must recover after a cache wipe").toBe(1);
});
