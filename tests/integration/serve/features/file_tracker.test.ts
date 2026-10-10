/// File tracker: each case drives deterministic ticks through the
/// clice/internal/poll hook (loops disabled), whose event counts it
/// asserts. A workspace tick looks at every known file; a look finding
/// other bytes than the one before — the scan's included — is a change.

import * as fs from "node:fs";
import * as path from "node:path";
import { MTIME_GRANULARITY } from "@clice/tools/client";
import type { Serve, ServeOptions } from "@clice/tools/actions";
import { SAMPLES_DIR, readManifest, type Manifest } from "@clice/tools/project";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

const SHAPES = readManifest("shapes/headers");
const FAST = SHAPES.files!["fast"]!;
const REGISTRY = SHAPES.files!["registry"]!;

/// The shapes database with fast.cpp built under `args` alone.
function fastUnder(...args: string[]): Manifest {
    return { ...SHAPES, units: { ...SHAPES.units, [FAST]: args } };
}

/// fast.cpp built without SHAPES_FAST: its #error fires.
const FAST_UNSET: ServeOptions = { units: { [FAST]: [] } };

/// Inserted into the config header, turns the fast mode off for every
/// includer: fast.cpp then defines exact_precision, not fast_precision.
const FAST_OFF = "#undef SHAPES_FAST\n#define SHAPES_FAST 0\n";

const NO_INDEX = { config: { project: { enable_indexing: false } } };

const HEADER_V1 = `#define VALUE 1
#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const HEADER_V2 = `#define VALUE 2
#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const CXX = ["-std=c++17"];

/// Every source a C++17 unit with `args`.
function manifest(sources: string[], args: string[] = []): Manifest {
    return { cxx: CXX, units: Object.fromEntries(sources.map((source) => [source, args])) };
}

/// Every source a C++17 unit with `args`, `options` besides.
function cxx17(sources: string[], args: string[] = [], options: ServeOptions = {}): ServeOptions {
    return { ...options, manifest: manifest(sources, args) };
}

/// The files whose rows reference the symbol at `loc`.
async function referrers(s: Serve, loc: Loc): Promise<string[]> {
    return [...new Set(((await s.references(loc)) ?? []).map((site) => s.relative(site.uri)))];
}

async function indexes(s: Serve, name: string): Promise<boolean> {
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    return symbols.some((symbol) => symbol.name === name);
}

/// Write `file` back with the sample's text, after the case removed it.
function restore(s: Serve, file: string): void {
    s.disk.write(file, fs.readFileSync(path.join(SAMPLES_DIR, s.project!, file), "utf8"));
}

/// The detail header is gone when the server starts.
const NO_DETAIL: ServeOptions = {
    setup: (workspace) => {
        workspace.rm(SHAPES.files!["detail"]!);
    },
};

serve("shapes/headers", FAST_UNSET)("cdb flag change recompiles", async ({ s }) => {
    expect(await s.errors(s.file("fast")), "gate must fire without -DSHAPES_FAST").not.toEqual([]);

    s.disk.database(SHAPES);
    expect(await s.poll("cdb")).toBe(1);
    expect(await s.errors(s.file("fast")), "open file must pick up the new flags").toEqual([]);
});

serve("shapes/headers", FAST_UNSET)("cdb stamped tick settles", async ({ s }) => {
    const stamped = { force: false };
    expect(await s.poll("cdb", stamped), "unchanged stamp must be quiet").toBe(0);

    s.disk.database(SHAPES);
    expect(await s.poll("cdb", stamped), "a fresh stamp only arms the settling debounce").toBe(0);
    expect(await s.poll("cdb", stamped), "the settled stamp reloads").toBe(1);
});

serve("shapes/headers", { units: { [REGISTRY]: null } })("cdb new entry indexed", async ({ s }) => {
    await s.compiled(s.file("main"));
    await s.indexed();
    expect(await indexes(s, "registry_reset"), "a source outside the CDB is indexed").toBe(false);

    s.disk.database(SHAPES);
    expect(await s.poll("cdb")).toBe(1);

    await s.indexed();
    expect(await indexes(s, "registry_reset"), "file added to the CDB was never indexed").toBe(
        true,
    );
});

serve("shapes/headers")("cdb removed entry recheck", async ({ s }) => {
    let result = await s.contexts(s.file("circle"));
    expect(result.total, "its includers must host the header initially").toBeGreaterThanOrEqual(1);

    s.disk.database({ units: {} });
    expect(await s.poll("cdb")).toBe(1);

    result = await s.contexts(s.file("circle"));
    expect(result.total, "removed entries must stop hosting the header").toBe(0);
});

serve("shapes/headers", { databases: false })("cdb appears after startup", async ({ s }) => {
    expect(await s.errors(s.file("fast")), "guessed command cannot define SHAPES_FAST").not.toEqual(
        [],
    );

    // The editor was opened first; cmake runs later.
    s.disk.database(s.manifest);
    expect(await s.poll("cdb")).toBe(1);

    expect(await s.errors(s.file("fast")), "open file must switch to the discovered CDB").toEqual(
        [],
    );
    await s.indexed();
    expect(
        await indexes(s, "registry_reset"),
        "closed file from the discovered CDB was never indexed",
    ).toBe(true);
});

serve("shapes/headers")("checkout updates workspace", async ({ s }) => {
    const area = at(s.file("circle"), "double are|a");
    await s.clean(s.file("main"));
    await s.indexed();
    expect(
        await referrers(s, area),
        "initial index never resolved the closed TU's area call",
    ).toContain(s.file("circle_impl"));

    expect(await s.poll("workspace")).toBe(0);

    // Simulate git checkout of a rename: rewrite files on disk, no didSave.
    s.disk.edit(s.file("circle"), { replace: "double area(", with: "double area_of(" });
    s.disk.edit(
        s.file("circle_impl"),
        { replace: "return area(", with: "return area_of(" },
        { replace: "double area(", with: "double area_of(" },
    );
    expect(await s.poll("workspace")).toBe(2);

    expect(
        await s.errors(s.file("main")),
        "open file must compile against the new header",
    ).not.toEqual([]);
    await s.indexed();
    const sites = s.show(await s.references(area));
    expect(sites, "closed TU was not reindexed against the new header").toContain(
        "src/circle.cpp: return area_of(*this);",
    );
    expect(sites, "closed TU's own disk change was not indexed").toContain(
        "src/circle.cpp: double area_of(const Circle& circle) {",
    );
});

serve("shapes/headers")("checkout under an open header", async ({ s }) => {
    await s.indexed();
    expect(await indexes(s, "fast_precision")).toBe(true);
    s.open(s.file("config"));
    expect(await s.poll("workspace")).toBe(0);

    // The editor reloads a clean buffer after a checkout: didChange, no
    // didSave. The buffer shadows the disk for the header's own compile
    // only, so the closed includer sees the checkout while it stays open.
    s.disk.edit(s.file("config"), { after: "#define SHAPES_API\n", insert: FAST_OFF });
    s.edit(s.file("config"), { after: "#define SHAPES_API\n", insert: FAST_OFF });
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    expect(
        await indexes(s, "exact_precision"),
        "closed TU was not reindexed while the header stayed open",
    ).toBe(true);
});

serve.files(
    {
        "header.h": HEADER_V1,
        "closed.cpp":
            '#define HEADER "header.h"\n#include HEADER\nint use_target() { return TARGET(); }\n',
    },
    cxx17(["closed.cpp"]),
)("macro include change reindexes", async ({ s }) => {
    await s.indexed();
    expect(await referrers(s, at("header.h", "alpha()"))).toContain("closed.cpp");
    expect(await s.poll("workspace")).toBe(0);

    // Only the compile resolves the include: the header is watched and its
    // includer found through what the indexed compile read.
    s.disk.write("header.h", HEADER_V2);
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    expect(
        await referrers(s, at("header.h", "beta()")),
        "the macro includer was not reindexed",
    ).toContain("closed.cpp");
});

// Where a failed include looked is watched: creating the header there
// recompiles the open includer and reindexes the closed one.
serve("shapes/headers", NO_DETAIL)("created header reaches includers", async ({ s }) => {
    expect(await s.errors(s.file("demo")), "the detail header does not exist yet").not.toEqual([]);
    await s.indexed();
    expect(await referrers(s, at(s.file("circle"), "double are|a("))).toContain(
        s.file("circle_impl"),
    );

    restore(s, s.file("detail"));
    expect(await s.poll("workspace")).toBe(1);
    expect(await s.errors(s.file("demo")), "the open includer must find the new header").toEqual(
        [],
    );
    await s.indexed();
    expect(
        await referrers(s, at(s.file("detail"), "square(double")),
        "the closed includer was not reindexed",
    ).toContain(s.file("circle_impl"));
});

// The includer's index predates the header: its own run must not take
// that index's word that the includer never enters it.
serve("shapes/headers", NO_DETAIL)("created header indexes in its includer", async ({ s }) => {
    await s.indexed();
    restore(s, s.file("detail"));
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    expect(await referrers(s, at(s.file("detail"), "square(double"))).toContain(
        s.file("circle_impl"),
    );
});

serve("shapes/headers")("dependency change keeps buffer rows", async ({ s }) => {
    await s.compiled(s.file("main"));
    await s.indexed();
    expect(await indexes(s, "demo")).toBe(true);
    await s.compiled(s.file("demo"));
    s.edit(s.file("demo"), { after: "square(2.0);\n}\n", insert: "// unsaved\n" });
    await s.compiled(s.file("demo"));
    const demoSites = async () =>
        ((await s.references(at(s.file("main"), "registry_count()"))) ?? []).filter(
            (site) => s.relative(site.uri) === s.file("demo"),
        ).length;
    expect(await demoSites()).toBe(1);

    // The header moves on disk: demo.cpp's compile is stale, its buffer is
    // not, so the rows it compiled from these very bytes keep serving.
    // Settled first, as in "delete while open reported".
    await s.sync();
    expect(await s.poll("workspace")).toBe(0);
    s.disk.edit(s.file("circle"), { after: "#pragma once\n", insert: "// moved\n" });
    expect(await s.poll("workspace")).toBe(1);
    expect(await demoSites(), "an edited buffer's rows vanished on a dependency change").toBe(1);
});

serve("shapes/headers")("touch emits no events", async ({ s }) => {
    const circle = s.file("circle");
    expect(await s.poll("workspace")).toBe(0);

    // mtime bump, identical bytes: the content-hash check must stay
    // silent.
    s.disk.write(circle, s.disk.read(circle));
    s.disk.touch(circle, new Date(s.disk.mtime(circle).getTime() + MTIME_GRANULARITY));
    expect(await s.poll("workspace")).toBe(0);
});

serve("shapes/headers", { ...FAST_UNSET, config: { tracker: { workspace_poll_seconds: 1 } } })(
    "cdb polling loop live",
    async ({ s }) => {
        expect(await s.errors(s.file("fast"))).not.toEqual([]);
        await s.indexed();

        // No hook: the polling loop reloads the database on its own, and the
        // reindex the new flags queue is the event waited for.
        const reindex = await s.hold("index", s.file("fast"));
        s.disk.database(SHAPES);
        await reindex.reached();
        await reindex.release();
        expect(
            await s.errors(s.file("fast")),
            "the polling loop must reload the CDB on its own",
        ).toEqual([]);
    },
);

serve("shapes/headers")("cdb flag change reindexes closed", async ({ s }) => {
    await s.compiled(s.file("main"));
    await s.indexed();
    expect(await indexes(s, "fast_precision"), "closed file was never indexed initially").toBe(
        true,
    );

    // Only fast.cpp's flags change; its bytes do not. Content-based
    // staleness cannot see this — the CDB delta must force the reindex.
    s.disk.database(fastUnder("-DSHAPES_FAST=0"));
    expect(await s.poll("cdb")).toBe(1);

    await s.indexed();
    expect(
        await indexes(s, "exact_precision"),
        "closed file was not reindexed after its flags changed",
    ).toBe(true);
});

serve("shapes/headers")("rewrite before first tick reported", async ({ s }) => {
    await s.indexed();
    expect(
        await indexes(s, "fast_precision"),
        "initial index never compiled the closed TU's fast branch",
    ).toBe(true);

    // No seeding tick: the first one judges the header against the bytes
    // the startup scan read.
    s.disk.edit(s.file("config"), { after: "#define SHAPES_API\n", insert: FAST_OFF });
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    expect(
        await indexes(s, "exact_precision"),
        "closed TU was not reindexed against the rewritten header",
    ).toBe(true);
});

serve("shapes/headers")("delete while open reported", async ({ s }) => {
    // A buffer shadows the disk for its own file's compile only: the
    // removal is its includers' news while the header is still open.
    s.open(s.file("circle"));
    // Settled: work still running would look at the header and report its
    // removal before the poll does.
    await s.sync();
    expect(await s.poll("workspace")).toBe(0);
    s.disk.rm(s.file("circle"));
    expect(await s.poll("workspace"), "an open file's removal is reported").toBe(1);
    s.close(s.file("circle"));
    expect(await s.poll("workspace"), "reported once").toBe(0);
});

serve("shapes/headers", NO_INDEX)("unchanged save no recompile", async ({ s }) => {
    const main = s.file("main");
    s.open(s.file("circle"));
    expect(await s.errors(main)).toEqual([]);

    // A recompile of the host counts a compile and publishes fresh
    // diagnostics; a hover on a clean AST does neither.
    const host = async () => {
        await s.sync();
        const builds = (await s.counts()).files[main];
        return { compile: builds?.compile, publish: builds?.publish };
    };
    const hover = () => s.hover(at(s.file("main"), "area(c)"));
    const settled = await host();
    await hover();
    expect(await host(), "control: a clean AST serves the hover").toEqual(settled);
    s.save(s.file("circle"));
    await hover();
    expect(await host(), "saving unchanged bytes must not dirty the host").toEqual(settled);
});

// A stamp the filesystem cannot vouch for: the mtime is not safely in the
// past, so an unchanged stat proves nothing about the bytes. The server
// must first read the database with it.
const FUTURE = new Date(Date.now() + 3_600_000);

serve("shapes/headers", {
    // As long as -DSHAPES_FAST, which the rewrite puts in its place.
    units: { [FAST]: ["-USHAPES_FAST"] },
    ...NO_INDEX,
    setup: (workspace) => {
        fs.utimesSync(workspace.path("compile_commands.json"), FUTURE, FUTURE);
    },
})("same stamp cdb rewrite applied", async ({ s }) => {
    const cdb = s.workspace.path("compile_commands.json");
    expect(await s.errors(s.file("fast"))).toHaveLength(1);

    const stamped = { force: false };
    expect(await s.poll("cdb", stamped)).toBe(0);
    // In place, same length, same mtime: only the content tells.
    const before = fs.statSync(cdb, { bigint: true });
    s.disk.database(fastUnder("-DSHAPES_FAST"));
    fs.utimesSync(cdb, FUTURE, FUTURE);
    const after = fs.statSync(cdb, { bigint: true });
    expect(after.size).toBe(before.size);
    expect(after.mtimeNs).toBe(before.mtimeNs);

    // The new content settles like any rewrite: seen on two polls.
    expect(await s.poll("cdb", stamped)).toBe(0);
    expect(await s.poll("cdb", stamped)).toBe(1);
    expect(await s.errors(s.file("fast")), "the rewritten flag must reach the open file").toEqual(
        [],
    );
});

/// Flags giving the TU a sysroot inside the workspace: the driver adds its
/// include directories itself, so the headers there count as installed
/// ones, like a toolchain's.
const SYSROOT = ["--target=x86_64-unknown-linux-gnu", "--sysroot=${workspace}/sysroot"];

serve.files(
    {
        "sysroot/usr/include/installed.h": "#define INSTALLED 1\n",
        "local.h": "#define LOCAL 1\n",
        "main.cpp":
            '#include <installed.h>\n#include "local.h"\nint main() { return INSTALLED + LOCAL; }\n',
    },
    // clang-tidy's configuration lookups would add their own looks.
    cxx17(["main.cpp"], SYSROOT, {
        config: { project: { enable_indexing: false }, diagnostics: { clang_tidy: false } },
    }),
)("requests look at workspace files only", async ({ s }) => {
    expect(await s.errors("main.cpp")).toEqual([]);
    const hover = () => s.hover(at("main.cpp", "main()"));
    await hover();

    const before = await s.stats();
    await hover();
    const after = await s.stats();
    expect(after.checksLooked - before.checksLooked, "the workspace header is looked at").toBe(1);
    expect(after.checksTrusted - before.checksTrusted, "the installed header is not").toBe(1);
});

// Finding the PCH stale, the request rebuilds it: the check, the PCH's
// preparation and its build share one look at each header.
serve("shapes/headers", NO_INDEX)("stale request looks once", async ({ s }) => {
    await s.compiled(s.file("main"));

    const looked = async (request: () => Promise<unknown>) => {
        const before = await s.stats();
        await request();
        return (await s.stats()).checksLooked - before.checksLooked;
    };
    const hover = () => s.hover(at(s.file("main"), "int |main()"));
    const completion = () => s.completion(at(s.file("main"), "return |static_cast"));
    const freshHover = await looked(hover);
    const freshCompletion = await looked(completion);

    s.disk.edit(s.file("polygon"), { after: "#pragma once\n", insert: "// rewritten\n" });
    expect(await looked(hover)).toBe(freshHover);
    s.disk.edit(s.file("circle"), { after: "#pragma once\n", insert: "// rewritten\n" });
    expect(await looked(completion)).toBe(freshCompletion);
});

serve.files(
    {
        "sysroot/usr/include/installed.h": "#define INSTALLED 1\n",
        "main.cpp": '#include <installed.h>\nstatic_assert(INSTALLED == 2, "");\n',
    },
    cxx17(["main.cpp"], SYSROOT, NO_INDEX),
)("save looks at installed headers", async ({ s }) => {
    expect(await s.errors("main.cpp"), "the installed header defines 1").not.toEqual([]);

    // An upgrade rewrites the installed header; nothing asks until a save.
    // Of the same size, the rewrite is told apart by a later mtime.
    const installed = "sysroot/usr/include/installed.h";
    s.disk.write(installed, "#define INSTALLED 2\n");
    s.disk.touch(installed, new Date(s.disk.mtime(installed).getTime() + MTIME_GRANULARITY));
    s.save("main.cpp");
    expect(await s.errors("main.cpp"), "the save must look at the installed header").toEqual([]);
});

serve("shapes/headers", { config: { tracker: { workspace_poll_seconds: 1 } } })(
    "background ticks see a rewrite",
    async ({ s }) => {
        await s.indexed();
        expect(await indexes(s, "fast_precision")).toBe(true);

        // No hook, no save, and the index answers without looking at the
        // disk: only a tick can see the rewrite, and the reindex it queues
        // is the event waited for.
        const reindex = await s.hold("index", s.file("fast"));
        s.disk.edit(s.file("config"), { after: "#define SHAPES_API\n", insert: FAST_OFF });
        await reindex.reached();
        await reindex.release();
        await s.indexed();
        expect(await indexes(s, "exact_precision"), "a background tick must see the rewrite").toBe(
            true,
        );
    },
);
